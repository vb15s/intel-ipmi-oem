/*
// Copyright (c) 2017 2018 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
*/

#include <boost/algorithm/string.hpp>
#include <boost/container/flat_map.hpp>
#include <chrono>
#include <cmath>
#include <commandutils.hpp>
#include <iostream>
#include <ipmid/api.hpp>
#include <ipmid/utils.hpp>
#include <phosphor-logging/log.hpp>
#include <sdbusplus/bus.hpp>
#include <sdrutils.hpp>
#include <sensorcommands.hpp>
#include <sensorutils.hpp>
#include <storagecommands.hpp>
#include <string>

namespace ipmi
{
using ManagedObjectType =
    std::map<sdbusplus::message::object_path,
             std::map<std::string, std::map<std::string, DbusVariant>>>;

using SensorMap = std::map<std::string, std::map<std::string, DbusVariant>>;
namespace variant_ns = sdbusplus::message::variant_ns;

static constexpr int sensorListUpdatePeriod = 10;
static constexpr int sensorMapUpdatePeriod = 2;

constexpr size_t maxSDRTotalSize =
    76; // Largest SDR Record Size (type 01) + SDR Overheader Size
constexpr static const uint32_t noTimestamp = 0xFFFFFFFF;

static uint16_t sdrReservationID;
static uint32_t sdrLastAdd = noTimestamp;
static uint32_t sdrLastRemove = noTimestamp;

SensorSubTree sensorTree;
static boost::container::flat_map<std::string, ManagedObjectType> SensorCache;

// Specify the comparison required to sort and find char* map objects
struct CmpStr
{
    bool operator()(const char *a, const char *b) const
    {
        return std::strcmp(a, b) < 0;
    }
};
const static boost::container::flat_map<const char *, SensorUnits, CmpStr>
    sensorUnits{{{"temperature", SensorUnits::degreesC},
                 {"voltage", SensorUnits::volts},
                 {"current", SensorUnits::amps},
                 {"fan_tach", SensorUnits::rpm},
                 {"power", SensorUnits::watts}}};

void registerSensorFunctions() __attribute__((constructor));
static sdbusplus::bus::bus dbus(ipmid_get_sd_bus_connection());

static sdbusplus::bus::match::match sensorAdded(
    dbus,
    "type='signal',member='InterfacesAdded',arg0path='/xyz/openbmc_project/"
    "sensors/'",
    [](sdbusplus::message::message &m) {
        sensorTree.clear();
        sdrLastAdd = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    });

static sdbusplus::bus::match::match sensorRemoved(
    dbus,
    "type='signal',member='InterfacesRemoved',arg0path='/xyz/openbmc_project/"
    "sensors/'",
    [](sdbusplus::message::message &m) {
        sensorTree.clear();
        sdrLastRemove = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    });

// this keeps track of deassertions for sensor event status command. A
// deasertion can only happen if an assertion was seen first.
static boost::container::flat_map<
    std::string, boost::container::flat_map<std::string, std::optional<bool>>>
    thresholdDeassertMap;

static sdbusplus::bus::match::match thresholdChanged(
    dbus,
    "type='signal',member='PropertiesChanged',interface='org.freedesktop.DBus."
    "Properties',arg0namespace='xyz.openbmc_project.Sensor.Threshold'",
    [](sdbusplus::message::message &m) {
        boost::container::flat_map<std::string, std::variant<bool, double>>
            values;
        m.read(std::string(), values);

        auto findAssert =
            std::find_if(values.begin(), values.end(), [](const auto &pair) {
                return pair.first.find("Alarm") != std::string::npos;
            });
        if (findAssert != values.end())
        {
            auto ptr = std::get_if<bool>(&(findAssert->second));
            if (ptr == nullptr)
            {
                phosphor::logging::log<phosphor::logging::level::ERR>(
                    "thresholdChanged: Assert non bool");
                return;
            }
            if (*ptr)
            {
                phosphor::logging::log<phosphor::logging::level::INFO>(
                    "thresholdChanged: Assert",
                    phosphor::logging::entry("SENSOR=%s", m.get_path()));
                thresholdDeassertMap[m.get_path()][findAssert->first] = *ptr;
            }
            else
            {
                auto &value =
                    thresholdDeassertMap[m.get_path()][findAssert->first];
                if (value)
                {
                    phosphor::logging::log<phosphor::logging::level::INFO>(
                        "thresholdChanged: deassert",
                        phosphor::logging::entry("SENSOR=%s", m.get_path()));
                    value = *ptr;
                }
            }
        }
    });

static void
    getSensorMaxMin(const std::map<std::string, DbusVariant> &sensorPropertyMap,
                    double &max, double &min)
{
    auto maxMap = sensorPropertyMap.find("MaxValue");
    auto minMap = sensorPropertyMap.find("MinValue");
    max = 127;
    min = -128;

    if (maxMap != sensorPropertyMap.end())
    {
        max = variant_ns::visit(VariantToDoubleVisitor(), maxMap->second);
    }
    if (minMap != sensorPropertyMap.end())
    {
        min = variant_ns::visit(VariantToDoubleVisitor(), minMap->second);
    }
}

static bool getSensorMap(std::string sensorConnection, std::string sensorPath,
                         SensorMap &sensorMap)
{
    static boost::container::flat_map<
        std::string, std::chrono::time_point<std::chrono::steady_clock>>
        updateTimeMap;

    auto updateFind = updateTimeMap.find(sensorConnection);
    auto lastUpdate = std::chrono::time_point<std::chrono::steady_clock>();
    if (updateFind != updateTimeMap.end())
    {
        lastUpdate = updateFind->second;
    }

    auto now = std::chrono::steady_clock::now();

    if (std::chrono::duration_cast<std::chrono::seconds>(now - lastUpdate)
            .count() > sensorMapUpdatePeriod)
    {
        updateTimeMap[sensorConnection] = now;

        auto managedObj = dbus.new_method_call(
            sensorConnection.c_str(), "/", "org.freedesktop.DBus.ObjectManager",
            "GetManagedObjects");

        ManagedObjectType managedObjects;
        try
        {
            auto reply = dbus.call(managedObj);
            reply.read(managedObjects);
        }
        catch (sdbusplus::exception_t &)
        {
            phosphor::logging::log<phosphor::logging::level::ERR>(
                "Error getting managed objects from connection",
                phosphor::logging::entry("CONNECTION=%s",
                                         sensorConnection.c_str()));
            return false;
        }

        SensorCache[sensorConnection] = managedObjects;
    }
    auto connection = SensorCache.find(sensorConnection);
    if (connection == SensorCache.end())
    {
        return false;
    }
    auto path = connection->second.find(sensorPath);
    if (path == connection->second.end())
    {
        return false;
    }
    sensorMap = path->second;

    return true;
}

/* sensor commands */
ipmi_ret_t ipmiSensorWildcardHandler(ipmi_netfn_t netfn, ipmi_cmd_t cmd,
                                     ipmi_request_t request,
                                     ipmi_response_t response,
                                     ipmi_data_len_t dataLen,
                                     ipmi_context_t context)
{
    *dataLen = 0;
    printCommand(+netfn, +cmd);
    return IPMI_CC_INVALID;
}

ipmi::RspType<> ipmiSenPlatformEvent(ipmi::message::Payload &p)
{
    uint8_t generatorID = 0;
    uint8_t evmRev = 0;
    uint8_t sensorType = 0;
    uint8_t sensorNum = 0;
    uint8_t eventType = 0;
    uint8_t eventData1 = 0;
    std::optional<uint8_t> eventData2 = 0;
    std::optional<uint8_t> eventData3 = 0;

    // todo: This check is supposed to be based on the incoming channel.
    //      e.g. system channel will provide upto 8 bytes including generator
    //      ID, but ipmb channel will provide only up to 7 bytes without the
    //      generator ID.
    // Support for this check is coming in future patches, so for now just base
    // it on if the first byte is the EvMRev (0x04).
    if (p.size() && p.data()[0] == 0x04)
    {
        p.unpack(evmRev, sensorType, sensorNum, eventType, eventData1,
                 eventData2, eventData3);
        // todo: the generator ID for this channel is supposed to come from the
        // IPMB requesters slave address. Support for this is coming in future
        // patches, so for now just assume it is coming from the ME (0x2C).
        generatorID = 0x2C;
    }
    else
    {
        p.unpack(generatorID, evmRev, sensorType, sensorNum, eventType,
                 eventData1, eventData2, eventData3);
    }
    if (!p.fullyUnpacked())
    {
        return ipmi::responseReqDataLenInvalid();
    }

    bool assert = eventType & directionMask ? false : true;
    std::vector<uint8_t> eventData;
    eventData.push_back(eventData1);
    eventData.push_back(eventData2.value_or(0xFF));
    eventData.push_back(eventData3.value_or(0xFF));

    std::string sensorPath = getPathFromSensorNumber(sensorNum);
    std::string service =
        ipmi::getService(dbus, ipmiSELAddInterface, ipmiSELPath);
    sdbusplus::message::message writeSEL = dbus.new_method_call(
        service.c_str(), ipmiSELPath, ipmiSELAddInterface, "IpmiSelAdd");
    writeSEL.append(ipmiSELAddMessage, sensorPath, eventData, assert,
                    static_cast<uint16_t>(generatorID));
    try
    {
        dbus.call(writeSEL);
    }
    catch (sdbusplus::exception_t &e)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(e.what());
        return ipmi::responseUnspecifiedError();
    }

    return ipmi::responseSuccess();
}

ipmi::RspType<uint8_t, uint8_t, uint8_t, std::optional<uint8_t>>
    ipmiSenGetSensorReading(uint8_t sensnum)
{
    std::string connection;
    std::string path;

    auto status = getSensorConnection(sensnum, connection, path);
    if (status)
    {
        return ipmi::response(status);
    }

    SensorMap sensorMap;
    if (!getSensorMap(connection, path, sensorMap))
    {
        return ipmi::responseResponseError();
    }
    auto sensorObject = sensorMap.find("xyz.openbmc_project.Sensor.Value");

    if (sensorObject == sensorMap.end() ||
        sensorObject->second.find("Value") == sensorObject->second.end())
    {
        return ipmi::responseResponseError();
    }
    auto &valueVariant = sensorObject->second["Value"];
    double reading = variant_ns::visit(VariantToDoubleVisitor(), valueVariant);

    double max;
    double min;
    getSensorMaxMin(sensorObject->second, max, min);

    int16_t mValue = 0;
    int16_t bValue = 0;
    int8_t rExp = 0;
    int8_t bExp = 0;
    bool bSigned = false;

    if (!getSensorAttributes(max, min, mValue, rExp, bValue, bExp, bSigned))
    {
        return ipmi::responseResponseError();
    }

    uint8_t value =
        scaleIPMIValueFromDouble(reading, mValue, rExp, bValue, bExp, bSigned);
    uint8_t operation =
        static_cast<uint8_t>(IPMISensorReadingByte2::sensorScanningEnable);
    operation |=
        static_cast<uint8_t>(IPMISensorReadingByte2::eventMessagesEnable);

    uint8_t thresholds = 0;

    auto warningObject =
        sensorMap.find("xyz.openbmc_project.Sensor.Threshold.Warning");
    if (warningObject != sensorMap.end())
    {
        auto alarmHigh = warningObject->second.find("WarningAlarmHigh");
        auto alarmLow = warningObject->second.find("WarningAlarmLow");
        if (alarmHigh != warningObject->second.end())
        {
            if (std::get<bool>(alarmHigh->second))
            {
                thresholds |= static_cast<uint8_t>(
                    IPMISensorReadingByte3::upperNonCritical);
            }
        }
        if (alarmLow != warningObject->second.end())
        {
            if (std::get<bool>(alarmLow->second))
            {
                thresholds |= static_cast<uint8_t>(
                    IPMISensorReadingByte3::lowerNonCritical);
            }
        }
    }

    auto criticalObject =
        sensorMap.find("xyz.openbmc_project.Sensor.Threshold.Critical");
    if (criticalObject != sensorMap.end())
    {
        auto alarmHigh = criticalObject->second.find("CriticalAlarmHigh");
        auto alarmLow = criticalObject->second.find("CriticalAlarmLow");
        if (alarmHigh != criticalObject->second.end())
        {
            if (std::get<bool>(alarmHigh->second))
            {
                thresholds |=
                    static_cast<uint8_t>(IPMISensorReadingByte3::upperCritical);
            }
        }
        if (alarmLow != criticalObject->second.end())
        {
            if (std::get<bool>(alarmLow->second))
            {
                thresholds |=
                    static_cast<uint8_t>(IPMISensorReadingByte3::lowerCritical);
            }
        }
    }

    // no discrete as of today so optional byte is never returned
    return ipmi::responseSuccess(value, operation, thresholds, std::nullopt);
}

ipmi_ret_t ipmiSenSetSensorThresholds(ipmi_netfn_t netfn, ipmi_cmd_t cmd,
                                      ipmi_request_t request,
                                      ipmi_response_t response,
                                      ipmi_data_len_t dataLen,
                                      ipmi_context_t context)
{
    if (*dataLen != 8)
    {
        *dataLen = 0;
        return IPMI_CC_REQ_DATA_LEN_INVALID;
    }
    *dataLen = 0;

    SensorThresholdReq *req = static_cast<SensorThresholdReq *>(request);

    // upper two bits reserved
    if (req->mask & 0xC0)
    {
        return IPMI_CC_INVALID_FIELD_REQUEST;
    }

    // lower nc and upper nc not suppported on any sensor
    if ((req->mask & static_cast<uint8_t>(
                         SensorThresholdReqEnable::setLowerNonRecoverable)) ||
        (req->mask & static_cast<uint8_t>(
                         SensorThresholdReqEnable::setUpperNonRecoverable)))
    {
        return IPMI_CC_INVALID_FIELD_REQUEST;
    }

    // if no bits are set in the mask, nothing to do
    if (!(req->mask))
    {
        return IPMI_CC_OK;
    }

    std::string connection;
    std::string path;

    ipmi_ret_t status = getSensorConnection(req->sensorNum, connection, path);
    if (status)
    {
        return status;
    }
    SensorMap sensorMap;
    if (!getSensorMap(connection, path, sensorMap))
    {
        return IPMI_CC_RESPONSE_ERROR;
    }

    auto sensorObject = sensorMap.find("xyz.openbmc_project.Sensor.Value");

    if (sensorObject == sensorMap.end())
    {
        return IPMI_CC_RESPONSE_ERROR;
    }
    double max = 0;
    double min = 0;
    getSensorMaxMin(sensorObject->second, max, min);

    int16_t mValue = 0;
    int16_t bValue = 0;
    int8_t rExp = 0;
    int8_t bExp = 0;
    bool bSigned = false;

    if (!getSensorAttributes(max, min, mValue, rExp, bValue, bExp, bSigned))
    {
        return IPMI_CC_RESPONSE_ERROR;
    }

    bool setLowerCritical =
        req->mask &
        static_cast<uint8_t>(SensorThresholdReqEnable::setLowerCritical);
    bool setUpperCritical =
        req->mask &
        static_cast<uint8_t>(SensorThresholdReqEnable::setUpperCritical);

    bool setLowerWarning =
        req->mask &
        static_cast<uint8_t>(SensorThresholdReqEnable::setLowerNonCritical);
    bool setUpperWarning =
        req->mask &
        static_cast<uint8_t>(SensorThresholdReqEnable::setUpperNonCritical);

    // store a vector of property name, value to set, and interface
    std::vector<std::tuple<std::string, uint8_t, std::string>> thresholdsToSet;

    // define the indexes of the tuple
    constexpr uint8_t propertyName = 0;
    constexpr uint8_t thresholdValue = 1;
    constexpr uint8_t interface = 2;
    // verifiy all needed fields are present
    if (setLowerCritical || setUpperCritical)
    {
        auto findThreshold =
            sensorMap.find("xyz.openbmc_project.Sensor.Threshold.Critical");
        if (findThreshold == sensorMap.end())
        {
            return IPMI_CC_INVALID_FIELD_REQUEST;
        }
        if (setLowerCritical)
        {
            auto findLower = findThreshold->second.find("CriticalLow");
            if (findLower == findThreshold->second.end())
            {
                return IPMI_CC_INVALID_FIELD_REQUEST;
            }
            thresholdsToSet.emplace_back("CriticalLow", req->lowerCritical,
                                         findThreshold->first);
        }
        if (setUpperCritical)
        {
            auto findUpper = findThreshold->second.find("CriticalHigh");
            if (findUpper == findThreshold->second.end())
            {
                return IPMI_CC_INVALID_FIELD_REQUEST;
            }
            thresholdsToSet.emplace_back("CriticalHigh", req->upperCritical,
                                         findThreshold->first);
        }
    }
    if (setLowerWarning || setUpperWarning)
    {
        auto findThreshold =
            sensorMap.find("xyz.openbmc_project.Sensor.Threshold.Warning");
        if (findThreshold == sensorMap.end())
        {
            return IPMI_CC_INVALID_FIELD_REQUEST;
        }
        if (setLowerWarning)
        {
            auto findLower = findThreshold->second.find("WarningLow");
            if (findLower == findThreshold->second.end())
            {
                return IPMI_CC_INVALID_FIELD_REQUEST;
            }
            thresholdsToSet.emplace_back("WarningLow", req->lowerNonCritical,
                                         findThreshold->first);
        }
        if (setUpperWarning)
        {
            auto findUpper = findThreshold->second.find("WarningHigh");
            if (findUpper == findThreshold->second.end())
            {
                return IPMI_CC_INVALID_FIELD_REQUEST;
            }
            thresholdsToSet.emplace_back("WarningHigh", req->upperNonCritical,
                                         findThreshold->first);
        }
    }

    for (const auto &property : thresholdsToSet)
    {
        // from section 36.3 in the IPMI Spec, assume all linear
        double valueToSet = ((mValue * std::get<thresholdValue>(property)) +
                             (bValue * std::pow(10, bExp))) *
                            std::pow(10, rExp);
        setDbusProperty(dbus, connection, path, std::get<interface>(property),
                        std::get<propertyName>(property),
                        ipmi::Value(valueToSet));
    }

    return IPMI_CC_OK;
}

IPMIThresholds getIPMIThresholds(const SensorMap &sensorMap)
{
    IPMIThresholds resp;
    auto warningInterface =
        sensorMap.find("xyz.openbmc_project.Sensor.Threshold.Warning");
    auto criticalInterface =
        sensorMap.find("xyz.openbmc_project.Sensor.Threshold.Critical");

    if ((warningInterface != sensorMap.end()) ||
        (criticalInterface != sensorMap.end()))
    {
        auto sensorPair = sensorMap.find("xyz.openbmc_project.Sensor.Value");

        if (sensorPair == sensorMap.end())
        {
            // should not have been able to find a sensor not implementing
            // the sensor object
            throw std::runtime_error("Invalid sensor map");
        }

        double max;
        double min;
        getSensorMaxMin(sensorPair->second, max, min);

        int16_t mValue = 0;
        int16_t bValue = 0;
        int8_t rExp = 0;
        int8_t bExp = 0;
        bool bSigned = false;

        if (!getSensorAttributes(max, min, mValue, rExp, bValue, bExp, bSigned))
        {
            throw std::runtime_error("Invalid sensor atrributes");
        }
        if (warningInterface != sensorMap.end())
        {
            auto &warningMap = warningInterface->second;

            auto warningHigh = warningMap.find("WarningHigh");
            auto warningLow = warningMap.find("WarningLow");

            if (warningHigh != warningMap.end())
            {

                double value = variant_ns::visit(VariantToDoubleVisitor(),
                                                 warningHigh->second);
                resp.warningHigh = scaleIPMIValueFromDouble(
                    value, mValue, rExp, bValue, bExp, bSigned);
            }
            if (warningLow != warningMap.end())
            {
                double value = variant_ns::visit(VariantToDoubleVisitor(),
                                                 warningLow->second);
                resp.warningLow = scaleIPMIValueFromDouble(
                    value, mValue, rExp, bValue, bExp, bSigned);
            }
        }
        if (criticalInterface != sensorMap.end())
        {
            auto &criticalMap = criticalInterface->second;

            auto criticalHigh = criticalMap.find("CriticalHigh");
            auto criticalLow = criticalMap.find("CriticalLow");

            if (criticalHigh != criticalMap.end())
            {
                double value = variant_ns::visit(VariantToDoubleVisitor(),
                                                 criticalHigh->second);
                resp.criticalHigh = scaleIPMIValueFromDouble(
                    value, mValue, rExp, bValue, bExp, bSigned);
            }
            if (criticalLow != criticalMap.end())
            {
                double value = variant_ns::visit(VariantToDoubleVisitor(),
                                                 criticalLow->second);
                resp.criticalLow = scaleIPMIValueFromDouble(
                    value, mValue, rExp, bValue, bExp, bSigned);
            }
        }
    }
    return resp;
}

ipmi::RspType<uint8_t, // readable
              uint8_t, // lowerNCrit
              uint8_t, // lowerCrit
              uint8_t, // lowerNrecoverable
              uint8_t, // upperNC
              uint8_t, // upperCrit
              uint8_t> // upperNRecoverable
    ipmiSenGetSensorThresholds(uint8_t sensorNumber)
{
    std::string connection;
    std::string path;

    auto status = getSensorConnection(sensorNumber, connection, path);
    if (status)
    {
        return ipmi::response(status);
    }

    SensorMap sensorMap;
    if (!getSensorMap(connection, path, sensorMap))
    {
        return ipmi::responseResponseError();
    }

    IPMIThresholds thresholdData;
    try
    {
        thresholdData = getIPMIThresholds(sensorMap);
    }
    catch (std::exception &)
    {
        return ipmi::responseResponseError();
    }

    uint8_t readable = 0;
    uint8_t lowerNC = 0;
    uint8_t lowerCritical = 0;
    uint8_t lowerNonRecoverable = 0;
    uint8_t upperNC = 0;
    uint8_t upperCritical = 0;
    uint8_t upperNonRecoverable = 0;

    if (thresholdData.warningHigh)
    {
        readable |=
            1 << static_cast<uint8_t>(IPMIThresholdRespBits::upperNonCritical);
        upperNC = *thresholdData.warningHigh;
    }
    if (thresholdData.warningLow)
    {
        readable |=
            1 << static_cast<uint8_t>(IPMIThresholdRespBits::lowerNonCritical);
        lowerNC = *thresholdData.warningLow;
    }

    if (thresholdData.criticalHigh)
    {
        readable |=
            1 << static_cast<uint8_t>(IPMIThresholdRespBits::upperCritical);
        upperCritical = *thresholdData.criticalHigh;
    }
    if (thresholdData.criticalLow)
    {
        readable |=
            1 << static_cast<uint8_t>(IPMIThresholdRespBits::lowerCritical);
        lowerCritical = *thresholdData.criticalLow;
    }

    return ipmi::responseSuccess(readable, lowerNC, lowerCritical,
                                 lowerNonRecoverable, upperNC, upperCritical,
                                 upperNonRecoverable);
}

ipmi_ret_t ipmiSenGetSensorEventEnable(ipmi_netfn_t netfn, ipmi_cmd_t cmd,
                                       ipmi_request_t request,
                                       ipmi_response_t response,
                                       ipmi_data_len_t dataLen,
                                       ipmi_context_t context)
{
    if (*dataLen != 1)
    {
        *dataLen = 0;
        return IPMI_CC_REQ_DATA_LEN_INVALID;
    }
    *dataLen = 0; // default to 0 in case of an error

    uint8_t sensnum = *(static_cast<uint8_t *>(request));

    std::string connection;
    std::string path;

    auto status = getSensorConnection(sensnum, connection, path);
    if (status)
    {
        return status;
    }

    SensorMap sensorMap;
    if (!getSensorMap(connection, path, sensorMap))
    {
        return IPMI_CC_RESPONSE_ERROR;
    }

    auto warningInterface =
        sensorMap.find("xyz.openbmc_project.Sensor.Threshold.Warning");
    auto criticalInterface =
        sensorMap.find("xyz.openbmc_project.Sensor.Threshold.Critical");

    if ((warningInterface != sensorMap.end()) ||
        (criticalInterface != sensorMap.end()))
    {
        // zero out response buff
        auto responseClear = static_cast<uint8_t *>(response);
        std::fill(responseClear, responseClear + sizeof(SensorEventEnableResp),
                  0);

        // assume all threshold sensors
        auto resp = static_cast<SensorEventEnableResp *>(response);

        resp->enabled = static_cast<uint8_t>(
            IPMISensorEventEnableByte2::sensorScanningEnable);
        if (warningInterface != sensorMap.end())
        {
            auto &warningMap = warningInterface->second;

            auto warningHigh = warningMap.find("WarningHigh");
            auto warningLow = warningMap.find("WarningLow");
            if (warningHigh != warningMap.end())
            {
                resp->assertionEnabledLSB |= static_cast<uint8_t>(
                    IPMISensorEventEnableThresholds::upperNonCriticalGoingHigh);
                resp->deassertionEnabledLSB |= static_cast<uint8_t>(
                    IPMISensorEventEnableThresholds::upperNonCriticalGoingLow);
            }
            if (warningLow != warningMap.end())
            {
                resp->assertionEnabledLSB |= static_cast<uint8_t>(
                    IPMISensorEventEnableThresholds::lowerNonCriticalGoingLow);
                resp->deassertionEnabledLSB |= static_cast<uint8_t>(
                    IPMISensorEventEnableThresholds::lowerNonCriticalGoingHigh);
            }
        }
        if (criticalInterface != sensorMap.end())
        {
            auto &criticalMap = criticalInterface->second;

            auto criticalHigh = criticalMap.find("CriticalHigh");
            auto criticalLow = criticalMap.find("CriticalLow");

            if (criticalHigh != criticalMap.end())
            {
                resp->assertionEnabledMSB |= static_cast<uint8_t>(
                    IPMISensorEventEnableThresholds::upperCriticalGoingHigh);
                resp->deassertionEnabledMSB |= static_cast<uint8_t>(
                    IPMISensorEventEnableThresholds::upperCriticalGoingLow);
            }
            if (criticalLow != criticalMap.end())
            {
                resp->assertionEnabledLSB |= static_cast<uint8_t>(
                    IPMISensorEventEnableThresholds::lowerCriticalGoingLow);
                resp->deassertionEnabledLSB |= static_cast<uint8_t>(
                    IPMISensorEventEnableThresholds::lowerCriticalGoingHigh);
            }
        }
        *dataLen =
            sizeof(SensorEventEnableResp); // todo only return needed bytes
    }
    // no thresholds enabled
    else
    {
        *dataLen = 1;
        auto resp = static_cast<uint8_t *>(response);
        *resp = static_cast<uint8_t>(
            IPMISensorEventEnableByte2::eventMessagesEnable);
        *resp |= static_cast<uint8_t>(
            IPMISensorEventEnableByte2::sensorScanningEnable);
    }
    return IPMI_CC_OK;
}

ipmi_ret_t ipmiSenGetSensorEventStatus(ipmi_netfn_t netfn, ipmi_cmd_t cmd,
                                       ipmi_request_t request,
                                       ipmi_response_t response,
                                       ipmi_data_len_t dataLen,
                                       ipmi_context_t context)
{
    if (*dataLen != 1)
    {
        *dataLen = 0;
        return IPMI_CC_REQ_DATA_LEN_INVALID;
    }
    *dataLen = 0; // default to 0 in case of an error

    uint8_t sensnum = *(static_cast<uint8_t *>(request));

    std::string connection;
    std::string path;

    auto status = getSensorConnection(sensnum, connection, path);
    if (status)
    {
        return status;
    }

    SensorMap sensorMap;
    if (!getSensorMap(connection, path, sensorMap))
    {
        return IPMI_CC_RESPONSE_ERROR;
    }

    auto warningInterface =
        sensorMap.find("xyz.openbmc_project.Sensor.Threshold.Warning");
    auto criticalInterface =
        sensorMap.find("xyz.openbmc_project.Sensor.Threshold.Critical");

    // zero out response buff
    auto responseClear = static_cast<uint8_t *>(response);
    std::fill(responseClear, responseClear + sizeof(SensorEventStatusResp), 0);
    auto resp = static_cast<SensorEventStatusResp *>(response);
    resp->enabled =
        static_cast<uint8_t>(IPMISensorEventEnableByte2::sensorScanningEnable);

    std::optional<bool> criticalDeassertHigh =
        thresholdDeassertMap[path]["CriticalAlarmHigh"];
    std::optional<bool> criticalDeassertLow =
        thresholdDeassertMap[path]["CriticalAlarmLow"];
    std::optional<bool> warningDeassertHigh =
        thresholdDeassertMap[path]["WarningAlarmHigh"];
    std::optional<bool> warningDeassertLow =
        thresholdDeassertMap[path]["WarningAlarmLow"];

    if (criticalDeassertHigh && !*criticalDeassertHigh)
    {
        resp->deassertionsMSB |= static_cast<uint8_t>(
            IPMISensorEventEnableThresholds::upperCriticalGoingHigh);
    }
    if (criticalDeassertLow && !*criticalDeassertLow)
    {
        resp->deassertionsMSB |= static_cast<uint8_t>(
            IPMISensorEventEnableThresholds::upperCriticalGoingLow);
    }
    if (warningDeassertHigh && !*warningDeassertHigh)
    {
        resp->deassertionsLSB |= static_cast<uint8_t>(
            IPMISensorEventEnableThresholds::upperNonCriticalGoingHigh);
    }
    if (warningDeassertLow && !*warningDeassertLow)
    {
        resp->deassertionsLSB |= static_cast<uint8_t>(
            IPMISensorEventEnableThresholds::lowerNonCriticalGoingHigh);
    }

    if ((warningInterface != sensorMap.end()) ||
        (criticalInterface != sensorMap.end()))
    {
        resp->enabled = static_cast<uint8_t>(
            IPMISensorEventEnableByte2::eventMessagesEnable);
        if (warningInterface != sensorMap.end())
        {
            auto &warningMap = warningInterface->second;

            auto warningHigh = warningMap.find("WarningAlarmHigh");
            auto warningLow = warningMap.find("WarningAlarmLow");
            auto warningHighAlarm = false;
            auto warningLowAlarm = false;

            if (warningHigh != warningMap.end())
            {
                warningHighAlarm = sdbusplus::message::variant_ns::get<bool>(
                    warningHigh->second);
            }
            if (warningLow != warningMap.end())
            {
                warningLowAlarm = sdbusplus::message::variant_ns::get<bool>(
                    warningLow->second);
            }
            if (warningHighAlarm)
            {
                resp->assertionsLSB |= static_cast<uint8_t>(
                    IPMISensorEventEnableThresholds::upperNonCriticalGoingHigh);
            }
            if (warningLowAlarm)
            {
                resp->assertionsLSB |= 1; // lower nc going low
            }
        }
        if (criticalInterface != sensorMap.end())
        {
            auto &criticalMap = criticalInterface->second;

            auto criticalHigh = criticalMap.find("CriticalAlarmHigh");
            auto criticalLow = criticalMap.find("CriticalAlarmLow");
            auto criticalHighAlarm = false;
            auto criticalLowAlarm = false;

            if (criticalHigh != criticalMap.end())
            {
                criticalHighAlarm = sdbusplus::message::variant_ns::get<bool>(
                    criticalHigh->second);
            }
            if (criticalLow != criticalMap.end())
            {
                criticalLowAlarm = sdbusplus::message::variant_ns::get<bool>(
                    criticalLow->second);
            }
            if (criticalHighAlarm)
            {
                resp->assertionsMSB |= static_cast<uint8_t>(
                    IPMISensorEventEnableThresholds::upperCriticalGoingHigh);
            }
            if (criticalLowAlarm)
            {
                resp->assertionsLSB |= static_cast<uint8_t>(
                    IPMISensorEventEnableThresholds::lowerCriticalGoingLow);
            }
        }
        *dataLen = sizeof(SensorEventStatusResp);
    }

    // no thresholds enabled, don't need assertionMSB
    else
    {
        *dataLen = sizeof(SensorEventStatusResp) - 1;
    }

    return IPMI_CC_OK;
}

/* end sensor commands */

/* storage commands */

ipmi_ret_t ipmiStorageGetSDRRepositoryInfo(ipmi_netfn_t netfn, ipmi_cmd_t cmd,
                                           ipmi_request_t request,
                                           ipmi_response_t response,
                                           ipmi_data_len_t dataLen,
                                           ipmi_context_t context)
{
    printCommand(+netfn, +cmd);

    if (*dataLen)
    {
        *dataLen = 0;
        return IPMI_CC_REQ_DATA_LEN_INVALID;
    }
    *dataLen = 0; // default to 0 in case of an error

    if (sensorTree.empty() && !getSensorSubtree(sensorTree))
    {
        return IPMI_CC_RESPONSE_ERROR;
    }

    // zero out response buff
    auto responseClear = static_cast<uint8_t *>(response);
    std::fill(responseClear, responseClear + sizeof(GetSDRInfoResp), 0);

    auto resp = static_cast<GetSDRInfoResp *>(response);
    resp->sdrVersion = ipmiSdrVersion;
    uint16_t recordCount = sensorTree.size();

    // todo: for now, sdr count is number of sensors
    resp->recordCountLS = recordCount & 0xFF;
    resp->recordCountMS = recordCount >> 8;

    // free space unspcified
    resp->freeSpace[0] = 0xFF;
    resp->freeSpace[1] = 0xFF;

    resp->mostRecentAddition = sdrLastAdd;
    resp->mostRecentErase = sdrLastRemove;
    resp->operationSupport = static_cast<uint8_t>(
        SdrRepositoryInfoOps::overflow); // write not supported
    resp->operationSupport |=
        static_cast<uint8_t>(SdrRepositoryInfoOps::allocCommandSupported);
    resp->operationSupport |= static_cast<uint8_t>(
        SdrRepositoryInfoOps::reserveSDRRepositoryCommandSupported);
    *dataLen = sizeof(GetSDRInfoResp);
    return IPMI_CC_OK;
}

ipmi_ret_t ipmiStorageGetSDRAllocationInfo(ipmi_netfn_t netfn, ipmi_cmd_t cmd,
                                           ipmi_request_t request,
                                           ipmi_response_t response,
                                           ipmi_data_len_t dataLen,
                                           ipmi_context_t context)
{
    if (*dataLen)
    {
        *dataLen = 0;
        return IPMI_CC_REQ_DATA_LEN_INVALID;
    }
    *dataLen = 0; // default to 0 in case of an error
    GetAllocInfoResp *resp = static_cast<GetAllocInfoResp *>(response);

    // 0000h unspecified number of alloc units
    resp->allocUnitsLSB = 0;
    resp->allocUnitsMSB = 0;

    // max unit size is size of max record
    resp->allocUnitSizeLSB = maxSDRTotalSize & 0xFF;
    resp->allocUnitSizeMSB = maxSDRTotalSize >> 8;
    // read only sdr, no free alloc blocks
    resp->allocUnitFreeLSB = 0;
    resp->allocUnitFreeMSB = 0;
    resp->allocUnitLargestFreeLSB = 0;
    resp->allocUnitLargestFreeMSB = 0;
    // only allow one block at a time
    resp->maxRecordSize = 1;

    *dataLen = sizeof(GetAllocInfoResp);

    return IPMI_CC_OK;
}

ipmi_ret_t ipmiStorageReserveSDR(ipmi_netfn_t netfn, ipmi_cmd_t cmd,
                                 ipmi_request_t request,
                                 ipmi_response_t response,
                                 ipmi_data_len_t dataLen,
                                 ipmi_context_t context)
{
    printCommand(+netfn, +cmd);

    if (*dataLen)
    {
        *dataLen = 0;
        return IPMI_CC_REQ_DATA_LEN_INVALID;
    }
    *dataLen = 0; // default to 0 in case of an error
    sdrReservationID++;
    if (sdrReservationID == 0)
    {
        sdrReservationID++;
    }
    *dataLen = 2;
    auto resp = static_cast<uint8_t *>(response);
    resp[0] = sdrReservationID & 0xFF;
    resp[1] = sdrReservationID >> 8;

    return IPMI_CC_OK;
}

ipmi::RspType<uint16_t,            // next record ID
              std::vector<uint8_t> // payload
              >
    ipmiStorageGetSDR(uint16_t reservationID, uint16_t recordID, uint8_t offset,
                      uint8_t bytesToRead)
{
    constexpr uint16_t lastRecordIndex = 0xFFFF;

    // reservation required for partial reads with non zero offset into
    // record
    if ((sdrReservationID == 0 || reservationID != sdrReservationID) && offset)
    {
        return ipmi::responseInvalidReservationId();
    }

    if (sensorTree.empty() && !getSensorSubtree(sensorTree))
    {
        return ipmi::responseResponseError();
    }

    size_t fruCount = 0;
    ipmi_ret_t ret = ipmi::storage::getFruSdrCount(fruCount);
    if (ret != IPMI_CC_OK)
    {
        return ipmi::response(ret);
    }

    size_t lastRecord = sensorTree.size() + fruCount - 1;
    if (recordID == lastRecordIndex)
    {
        recordID = lastRecord;
    }
    if (recordID > lastRecord)
    {
        return ipmi::responseInvalidFieldRequest();
    }

    uint16_t nextRecordId = lastRecord > recordID ? recordID + 1 : 0XFFFF;

    if (recordID >= sensorTree.size())
    {
        size_t fruIndex = recordID - sensorTree.size();
        if (fruIndex >= fruCount)
        {
            return ipmi::responseInvalidFieldRequest();
        }
        get_sdr::SensorDataFruRecord data;
        if (offset > sizeof(data))
        {
            return ipmi::responseInvalidFieldRequest();
        }
        ret = ipmi::storage::getFruSdrs(fruIndex, data);
        if (ret != IPMI_CC_OK)
        {
            return ipmi::response(ret);
        }
        data.header.record_id_msb = recordID << 8;
        data.header.record_id_lsb = recordID & 0xFF;
        if (sizeof(data) < (offset + bytesToRead))
        {
            bytesToRead = sizeof(data) - offset;
        }

        uint8_t *respStart = reinterpret_cast<uint8_t *>(&data) + offset;
        std::vector<uint8_t> recordData(respStart, respStart + bytesToRead);

        return ipmi::responseSuccess(nextRecordId, recordData);
    }

    std::string connection;
    std::string path;
    uint16_t sensorIndex = recordID;
    for (const auto &sensor : sensorTree)
    {
        if (sensorIndex-- == 0)
        {
            if (!sensor.second.size())
            {
                return ipmi::responseResponseError();
            }
            connection = sensor.second.begin()->first;
            path = sensor.first;
            break;
        }
    }

    SensorMap sensorMap;
    if (!getSensorMap(connection, path, sensorMap))
    {
        return ipmi::responseResponseError();
    }
    uint8_t sensornumber = (recordID & 0xFF);
    get_sdr::SensorDataFullRecord record = {0};

    record.header.record_id_msb = recordID << 8;
    record.header.record_id_lsb = recordID & 0xFF;
    record.header.sdr_version = ipmiSdrVersion;
    record.header.record_type = get_sdr::SENSOR_DATA_FULL_RECORD;
    record.header.record_length = sizeof(get_sdr::SensorDataFullRecord) -
                                  sizeof(get_sdr::SensorDataRecordHeader);
    record.key.owner_id = 0x20;
    record.key.owner_lun = 0x0;
    record.key.sensor_number = sensornumber;

    record.body.entity_id = 0x0;
    record.body.entity_instance = 0x01;
    record.body.sensor_capabilities = 0x68; // auto rearm - todo hysteresis
    record.body.sensor_type = getSensorTypeFromPath(path);
    std::string type = getSensorTypeStringFromPath(path);
    auto typeCstr = type.c_str();
    auto findUnits = sensorUnits.find(typeCstr);
    if (findUnits != sensorUnits.end())
    {
        record.body.sensor_units_2_base =
            static_cast<uint8_t>(findUnits->second);
    } // else default 0x0 unspecified

    record.body.event_reading_type = getSensorEventTypeFromPath(path);

    auto sensorObject = sensorMap.find("xyz.openbmc_project.Sensor.Value");
    if (sensorObject == sensorMap.end())
    {
        return ipmi::responseResponseError();
    }

    auto maxObject = sensorObject->second.find("MaxValue");
    auto minObject = sensorObject->second.find("MinValue");
    double max = 128;
    double min = -127;
    if (maxObject != sensorObject->second.end())
    {
        max = variant_ns::visit(VariantToDoubleVisitor(), maxObject->second);
    }

    if (minObject != sensorObject->second.end())
    {
        min = variant_ns::visit(VariantToDoubleVisitor(), minObject->second);
    }

    int16_t mValue;
    int8_t rExp;
    int16_t bValue;
    int8_t bExp;
    bool bSigned;

    if (!getSensorAttributes(max, min, mValue, rExp, bValue, bExp, bSigned))
    {
        return ipmi::responseResponseError();
    }

    // apply M, B, and exponents, M and B are 10 bit values, exponents are 4
    record.body.m_lsb = mValue & 0xFF;

    // move the smallest bit of the MSB into place (bit 9)
    // the MSbs are bits 7:8 in m_msb_and_tolerance
    uint8_t mMsb = (mValue & (1 << 8)) > 0 ? (1 << 6) : 0;

    // assign the negative
    if (mValue < 0)
    {
        mMsb |= (1 << 7);
    }
    record.body.m_msb_and_tolerance = mMsb;

    record.body.b_lsb = bValue & 0xFF;

    // move the smallest bit of the MSB into place
    // the MSbs are bits 7:8 in b_msb_and_accuracy_lsb
    uint8_t bMsb = (bValue & (1 << 8)) > 0 ? (1 << 6) : 0;

    // assign the negative
    if (bValue < 0)
    {
        bMsb |= (1 << 7);
    }
    record.body.b_msb_and_accuracy_lsb = bMsb;

    record.body.r_b_exponents = bExp & 0x7;
    if (bExp < 0)
    {
        record.body.r_b_exponents |= 1 << 3;
    }
    record.body.r_b_exponents = (rExp & 0x7) << 4;
    if (rExp < 0)
    {
        record.body.r_b_exponents |= 1 << 7;
    }

    // todo fill out rest of units
    if (bSigned)
    {
        record.body.sensor_units_1 = 1 << 7;
    }

    // populate sensor name from path
    std::string name;
    size_t nameStart = path.rfind("/");
    if (nameStart != std::string::npos)
    {
        name = path.substr(nameStart + 1, std::string::npos - nameStart);
    }

    std::replace(name.begin(), name.end(), '_', ' ');
    if (name.size() > FULL_RECORD_ID_STR_MAX_LENGTH)
    {
        name.resize(FULL_RECORD_ID_STR_MAX_LENGTH);
    }
    record.body.id_string_info = name.size();
    std::strncpy(record.body.id_string, name.c_str(),
                 sizeof(record.body.id_string));

    IPMIThresholds thresholdData;
    try
    {
        thresholdData = getIPMIThresholds(sensorMap);
    }
    catch (std::exception &)
    {
        return ipmi::responseResponseError();
    }

    if (thresholdData.criticalHigh)
    {
        record.body.upper_critical_threshold = *thresholdData.criticalHigh;
        record.body.supported_deassertions[1] |= static_cast<uint8_t>(
            IPMISensorEventEnableThresholds::upperCriticalGoingHigh);
        record.body.supported_assertions[1] |= static_cast<uint8_t>(
            IPMISensorEventEnableThresholds::upperCriticalGoingHigh);
        record.body.discrete_reading_setting_mask[0] |=
            static_cast<uint8_t>(IPMISensorReadingByte3::upperCritical);
    }
    if (thresholdData.warningHigh)
    {
        record.body.upper_noncritical_threshold = *thresholdData.warningHigh;
        record.body.supported_deassertions[0] |= static_cast<uint8_t>(
            IPMISensorEventEnableThresholds::upperNonCriticalGoingHigh);
        record.body.supported_assertions[0] |= static_cast<uint8_t>(
            IPMISensorEventEnableThresholds::upperNonCriticalGoingHigh);
        record.body.discrete_reading_setting_mask[0] |=
            static_cast<uint8_t>(IPMISensorReadingByte3::upperNonCritical);
    }
    if (thresholdData.criticalLow)
    {
        record.body.lower_critical_threshold = *thresholdData.criticalLow;
        record.body.supported_deassertions[0] |= static_cast<uint8_t>(
            IPMISensorEventEnableThresholds::lowerCriticalGoingLow);
        record.body.supported_assertions[0] |= static_cast<uint8_t>(
            IPMISensorEventEnableThresholds::lowerCriticalGoingLow);
        record.body.discrete_reading_setting_mask[0] |=
            static_cast<uint8_t>(IPMISensorReadingByte3::lowerCritical);
    }
    if (thresholdData.warningLow)
    {
        record.body.lower_noncritical_threshold = *thresholdData.warningLow;
        record.body.supported_deassertions[0] |= static_cast<uint8_t>(
            IPMISensorEventEnableThresholds::lowerNonCriticalGoingLow);
        record.body.supported_assertions[0] |= static_cast<uint8_t>(
            IPMISensorEventEnableThresholds::lowerNonCriticalGoingLow);
        record.body.discrete_reading_setting_mask[0] |=
            static_cast<uint8_t>(IPMISensorReadingByte3::lowerNonCritical);
    }

    // everything that is readable is setable
    record.body.discrete_reading_setting_mask[1] =
        record.body.discrete_reading_setting_mask[0];

    if (sizeof(get_sdr::SensorDataFullRecord) < (offset + bytesToRead))
    {
        bytesToRead = sizeof(get_sdr::SensorDataFullRecord) - offset;
    }

    uint8_t *respStart = reinterpret_cast<uint8_t *>(&record) + offset;
    std::vector<uint8_t> recordData(respStart, respStart + bytesToRead);

    return ipmi::responseSuccess(nextRecordId, recordData);
}
/* end storage commands */

void registerSensorFunctions()
{
    // get firmware version information
    ipmiPrintAndRegister(NETFUN_SENSOR, IPMI_CMD_WILDCARD, nullptr,
                         ipmiSensorWildcardHandler, PRIVILEGE_USER);

    // <Get Sensor Type>
    ipmiPrintAndRegister(
        NETFUN_SENSOR,
        static_cast<ipmi_cmd_t>(IPMINetfnSensorCmds::ipmiCmdGetSensorType),
        nullptr, ipmiSensorWildcardHandler, PRIVILEGE_USER);

    // <Set Sensor Reading and Event Status>
    ipmiPrintAndRegister(
        NETFUN_SENSOR,
        static_cast<ipmi_cmd_t>(
            IPMINetfnSensorCmds::ipmiCmdSetSensorReadingAndEventStatus),
        nullptr, ipmiSensorWildcardHandler, PRIVILEGE_OPERATOR);

    // <Platform Event>
    ipmi::registerHandler(
        ipmi::prioOemBase, ipmi::netFnSensor,
        static_cast<ipmi::Cmd>(ipmi::sensor_event::cmdPlatformEvent),
        ipmi::Privilege::Operator, ipmiSenPlatformEvent);

    // <Get Sensor Reading>
    ipmi::registerHandler(
        ipmi::prioOemBase, NETFUN_SENSOR,
        static_cast<ipmi::Cmd>(IPMINetfnSensorCmds::ipmiCmdGetSensorReading),
        ipmi::Privilege::User, ipmiSenGetSensorReading);

    // <Get Sensor Threshold>
    ipmi::registerHandler(
        ipmi::prioOemBase, NETFUN_SENSOR,
        static_cast<ipmi::Cmd>(IPMINetfnSensorCmds::ipmiCmdGetSensorThreshold),
        ipmi::Privilege::User, ipmiSenGetSensorThresholds);

    ipmiPrintAndRegister(
        NETFUN_SENSOR,
        static_cast<ipmi_cmd_t>(IPMINetfnSensorCmds::ipmiCmdSetSensorThreshold),
        nullptr, ipmiSenSetSensorThresholds, PRIVILEGE_OPERATOR);

    // <Get Sensor Event Enable>
    ipmiPrintAndRegister(NETFUN_SENSOR,
                         static_cast<ipmi_cmd_t>(
                             IPMINetfnSensorCmds::ipmiCmdGetSensorEventEnable),
                         nullptr, ipmiSenGetSensorEventEnable, PRIVILEGE_USER);

    // <Get Sensor Event Status>
    ipmiPrintAndRegister(NETFUN_SENSOR,
                         static_cast<ipmi_cmd_t>(
                             IPMINetfnSensorCmds::ipmiCmdGetSensorEventStatus),
                         nullptr, ipmiSenGetSensorEventStatus, PRIVILEGE_USER);

    // register all storage commands for both Sensor and Storage command
    // versions

    // <Get SDR Repository Info>
    ipmiPrintAndRegister(
        NETFUN_STORAGE,
        static_cast<ipmi_cmd_t>(IPMINetfnStorageCmds::ipmiCmdGetRepositoryInfo),
        nullptr, ipmiStorageGetSDRRepositoryInfo, PRIVILEGE_USER);

    // <Get SDR Allocation Info>
    ipmiPrintAndRegister(NETFUN_STORAGE,
                         static_cast<ipmi_cmd_t>(
                             IPMINetfnStorageCmds::ipmiCmdGetSDRAllocationInfo),
                         nullptr, ipmiStorageGetSDRAllocationInfo,
                         PRIVILEGE_USER);

    // <Reserve SDR Repo>
    ipmiPrintAndRegister(NETFUN_SENSOR,
                         static_cast<ipmi_cmd_t>(
                             IPMINetfnSensorCmds::ipmiCmdReserveDeviceSDRRepo),
                         nullptr, ipmiStorageReserveSDR, PRIVILEGE_USER);

    ipmiPrintAndRegister(
        NETFUN_STORAGE,
        static_cast<ipmi_cmd_t>(IPMINetfnStorageCmds::ipmiCmdReserveSDR),
        nullptr, ipmiStorageReserveSDR, PRIVILEGE_USER);

    // <Get Sdr>
    ipmi::registerHandler(
        ipmi::prioOemBase, NETFUN_SENSOR,
        static_cast<ipmi::Cmd>(IPMINetfnSensorCmds::ipmiCmdGetDeviceSDR),
        ipmi::Privilege::User, ipmiStorageGetSDR);

    ipmi::registerHandler(
        ipmi::prioOemBase, NETFUN_STORAGE,
        static_cast<ipmi::Cmd>(IPMINetfnStorageCmds::ipmiCmdGetSDR),
        ipmi::Privilege::User, ipmiStorageGetSDR);

    return;
}
} // namespace ipmi
