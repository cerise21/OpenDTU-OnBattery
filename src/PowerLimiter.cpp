// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2022 Thomas Basler and others
 */

#include "Battery.h"
#include "PowerMeter.h"
#include "PowerLimiter.h"
#include "Configuration.h"
#include "MqttSettings.h"
#include "NetworkSettings.h"
#include <VeDirectFrameHandler.h>
#include "MessageOutput.h"
#include <ctime>

PowerLimiterClass PowerLimiter;

void PowerLimiterClass::init()
{
}

void PowerLimiterClass::loop()
{
    CONFIG_T& config = Configuration.get();

    if (!config.PowerLimiter_Enabled
            || !config.PowerMeter_Enabled
            || !Hoymiles.getRadio()->isIdle()
            || (millis() - _lastCommandSent) < (config.PowerLimiter_Interval * 1000)
            || (millis() - _lastLoop) < (config.PowerLimiter_Interval * 1000)) {
        if (!config.PowerLimiter_Enabled)
            _plState = STATE_DISCOVER; // ensure STATE_DISCOVER is set, if PowerLimiter will be enabled.
        return;
    }

    _lastLoop = millis();

    std::shared_ptr<InverterAbstract> inverter = Hoymiles.getInverterByPos(config.PowerLimiter_InverterId);
    if (inverter == nullptr || !inverter->isReachable()) {
        return;
    }

    float dcVoltage = inverter->Statistics()->getChannelFieldValue(TYPE_DC, (ChannelNum_t) config.PowerLimiter_InverterChannelId, FLD_UDC);
    float acPower = inverter->Statistics()->getChannelFieldValue(TYPE_AC, (ChannelNum_t) config.PowerLimiter_InverterChannelId, FLD_PAC);
    float correctedDcVoltage = dcVoltage + (acPower * config.PowerLimiter_VoltageLoadCorrectionFactor);

    // If the last inverter update is too old, don't do anything.
    // If the last inverter update was before the last limit updated, don't do anything.
    // Also give the Power meter 3 seconds time to recognize power changes because of the last set limit
    // and also because the Hoymiles MPPT might not react immediately.
    if ((millis() - inverter->Statistics()->getLastUpdate()) > 10000
            || inverter->Statistics()->getLastUpdate() <= _lastLimitSetTime
            || PowerMeter.getLastPowerMeterUpdate() <= (_lastLimitSetTime + 3000)) {
        return;
    }

    if (millis() - PowerMeter.getLastPowerMeterUpdate() < (30 * 1000)) {
        MessageOutput.printf("[PowerLimiterClass::loop] dcVoltage: %.2f Voltage Start Threshold: %.2f Voltage Stop Threshold: %.2f inverter->isProducing(): %d\r\n",
            dcVoltage, config.PowerLimiter_VoltageStartThreshold, config.PowerLimiter_VoltageStopThreshold, inverter->isProducing());
    }

    while(true) {
        switch(_plState) {
            case STATE_DISCOVER:
                if (!inverter->isProducing() || isStopThresholdReached(inverter)) {
                    _plState = STATE_OFF;
                }
                else if (canUseDirectSolarPower()) {
                    _plState = STATE_CONSUME_SOLAR_POWER_ONLY;
                }
                else {
                    _plState = STATE_NORMAL_OPERATION;
                }
                break;
            case STATE_OFF:
                // if on turn off
                if (inverter->isProducing()) {
                    MessageOutput.printf("[PowerLimiterClass::loop] DC voltage: %.2f Corrected DC voltage: %.2f...\r\n",
                        dcVoltage, correctedDcVoltage);
                    setNewPowerLimit(inverter, -1);
                    return;
                }

                // do nothing if battery is empty
                if (isStopThresholdReached(inverter)) 
                    return;
                // check for possible state changes
                if (canUseDirectSolarPower()) {
                    _plState = STATE_CONSUME_SOLAR_POWER_ONLY;
                }
                if (isStartThresholdReached(inverter)) {
                    _plState = STATE_NORMAL_OPERATION;
                }
                return;
                break;
            case STATE_CONSUME_SOLAR_POWER_ONLY: {
                int32_t newPowerLimit = calcPowerLimit(inverter, true);
                if (isStopThresholdReached(inverter)) {
                    _plState = STATE_OFF;
                    break;
                }
                if (isStartThresholdReached(inverter)) {
                    _plState = STATE_NORMAL_OPERATION;
                    break;
                }

                if (!canUseDirectSolarPower()) {
                    if (config.PowerLimiter_BatteryDrainStategy == EMPTY_AT_NIGHT)
                        _plState = STATE_NORMAL_OPERATION;
                    else
                        _plState = STATE_OFF;
                    break;
                }

                setNewPowerLimit(inverter, newPowerLimit);
                return;
                break;
            }
            case STATE_NORMAL_OPERATION: {
                int32_t newPowerLimit = calcPowerLimit(inverter, false);
                if (isStopThresholdReached(inverter)) {
                    _plState = STATE_OFF;
                    break;
                }
                if (!isStartThresholdReached(inverter) && canUseDirectSolarPower() && (config.PowerLimiter_BatteryDrainStategy == EMPTY_AT_NIGHT)) {
                    _plState = STATE_CONSUME_SOLAR_POWER_ONLY;
                    break;
                }

                // check if grid power consumption is not within the upper and lower threshold of the target consumption
                if (newPowerLimit >= (config.PowerLimiter_TargetPowerConsumption - config.PowerLimiter_TargetPowerConsumptionHysteresis) &&
                    newPowerLimit <= (config.PowerLimiter_TargetPowerConsumption + config.PowerLimiter_TargetPowerConsumptionHysteresis) &&
                    _lastRequestedPowerLimit >= (config.PowerLimiter_TargetPowerConsumption - config.PowerLimiter_TargetPowerConsumptionHysteresis) &&
                    _lastRequestedPowerLimit <= (config.PowerLimiter_TargetPowerConsumption + config.PowerLimiter_TargetPowerConsumptionHysteresis) ) {
                    return;    
                }
                setNewPowerLimit(inverter, newPowerLimit);;
                return;
                break;
            } 
        }
    }
}

plStates PowerLimiterClass::getPowerLimiterState() {
    return _plState;
}

int32_t PowerLimiterClass::getLastRequestedPowewrLimit() {
    return _lastRequestedPowerLimit;
}

bool PowerLimiterClass::canUseDirectSolarPower()
{
    CONFIG_T& config = Configuration.get();

    if (!config.PowerLimiter_SolarPassThroughEnabled
            || !config.Vedirect_Enabled) {
        return false;
    }

    if (VeDirect.veFrame.PPV < 20) {
        // Not enough power
        return false;
    }

    return true;
}

int32_t PowerLimiterClass::calcPowerLimit(std::shared_ptr<InverterAbstract> inverter, bool consumeSolarPowerOnly)
{
    CONFIG_T& config = Configuration.get();
    
    int32_t newPowerLimit = round(PowerMeter.getPowerTotal());

    // Safety check, return on too old power meter values
    if (millis() - PowerMeter.getLastPowerMeterUpdate() > (30 * 1000)
            && (millis() - inverter->Statistics()->getLastUpdate()) > (config.Dtu_PollInterval * 3 * 1000)) {
        // If the power meter values are older than 30 seconds, 
        // and the Inverter Stats are older then 3x the poll interval
        // set the limit to config.PowerLimiter_LowerPowerLimit for safety reasons.
        MessageOutput.println("[PowerLimiterClass::loop] Power Meter values too old. Using lower limit");
        return config.PowerLimiter_LowerPowerLimit;
    }

    // check if grid power consumption is within the limits of the target consumption + hysteresis
    if (newPowerLimit >= (config.PowerLimiter_TargetPowerConsumption - config.PowerLimiter_TargetPowerConsumptionHysteresis) &&
        newPowerLimit <= (config.PowerLimiter_TargetPowerConsumption + config.PowerLimiter_TargetPowerConsumptionHysteresis)) {
          // The values have not changed much. We just use the old setting
          MessageOutput.println("[PowerLimiterClass::loop] reusing old limit");
          return _lastRequestedPowerLimit;
    }

    if (config.PowerLimiter_IsInverterBehindPowerMeter) {
        // If the inverter the behind the power meter (part of measurement),
        // the produced power of this inverter has also to be taken into account.
        // We don't use FLD_PAC from the statistics, because that
        // data might be too old and unreliable.
        float acPower = inverter->Statistics()->getChannelFieldValue(TYPE_AC, (ChannelNum_t) config.PowerLimiter_InverterChannelId, FLD_PAC); 
        newPowerLimit += static_cast<int>(acPower);
    }

    float efficency = inverter->Statistics()->getChannelFieldValue(TYPE_AC, (ChannelNum_t) config.PowerLimiter_InverterChannelId, FLD_EFF);
    int32_t victronChargePower = this->getDirectSolarPower();
    int32_t adjustedVictronChargePower = victronChargePower * (efficency > 0.0 ? (efficency / 100.0) : 1.0); // if inverter is off, use 1.0

    MessageOutput.printf("[PowerLimiterClass::loop] victronChargePower: %d, efficiency: %.2f, consumeSolarPowerOnly: %s, powerConsumption: %d \r\n", 
        victronChargePower, efficency, consumeSolarPowerOnly ? "true" : "false", newPowerLimit);

    // We're not trying to hit 0 exactly but take an offset into account
    // This means we never fully compensate the used power with the inverter 
    newPowerLimit -= config.PowerLimiter_TargetPowerConsumption;

    int32_t upperPowerLimit = config.PowerLimiter_UpperPowerLimit;
    if (consumeSolarPowerOnly && (upperPowerLimit > adjustedVictronChargePower)) {
        // Battery voltage too low, use Victron solar power (corrected by efficiency factor) only
        upperPowerLimit = adjustedVictronChargePower;
    }

    if (newPowerLimit > upperPowerLimit) 
        newPowerLimit = upperPowerLimit;

    MessageOutput.printf("[PowerLimiterClass::loop] newPowerLimit: %d\r\n", newPowerLimit);
    return newPowerLimit;
}

void PowerLimiterClass::setNewPowerLimit(std::shared_ptr<InverterAbstract> inverter, int32_t newPowerLimit)
{
    CONFIG_T& config = Configuration.get();

    // Start the inverter in case it's inactive and if the requested power is high enough
    if (!inverter->isProducing() && newPowerLimit > config.PowerLimiter_LowerPowerLimit) {
        MessageOutput.println("[PowerLimiterClass::loop] Starting up inverter...");
        inverter->sendPowerControlRequest(Hoymiles.getRadio(), true);
        _lastCommandSent = millis();
    }

    // Stop the inverter if limit is below threshold.
    // We'll also set the power limit to the lower value in this case
    if (newPowerLimit < config.PowerLimiter_LowerPowerLimit) {
        if (inverter->isProducing()) {
            MessageOutput.println("[PowerLimiterClass::loop] Stopping inverter...");
            inverter->sendPowerControlRequest(Hoymiles.getRadio(), false);
            _lastCommandSent = millis();
        }
        newPowerLimit = config.PowerLimiter_LowerPowerLimit;
    }

    // Set the actual limit. We'll only do this is if the limit is in the right range
    // and differs from the last requested value
    if( _lastRequestedPowerLimit != newPowerLimit &&
          /* newPowerLimit > config.PowerLimiter_LowerPowerLimit &&  -->  This will always be true given the check above, kept for code readability */
          newPowerLimit < config.PowerLimiter_UpperPowerLimit ) {
        MessageOutput.printf("[PowerLimiterClass::loop] Limit Non-Persistent: %d W\r\n", newPowerLimit);
        inverter->sendActivePowerControlRequest(Hoymiles.getRadio(), newPowerLimit, PowerLimitControlType::AbsolutNonPersistent);
        _lastRequestedPowerLimit = newPowerLimit;
        // wait for the next inverter update (+ 3 seconds to make sure the limit got applied)
        _lastLimitSetTime = millis();
    }
}

int32_t PowerLimiterClass::getDirectSolarPower()
{
    if (!canUseDirectSolarPower()) {
        return 0;
    }

    return VeDirect.veFrame.PPV;
}

float PowerLimiterClass::getLoadCorrectedVoltage(std::shared_ptr<InverterAbstract> inverter)
{
    CONFIG_T& config = Configuration.get();

    float acPower = inverter->Statistics()->getChannelFieldValue(TYPE_AC, (ChannelNum_t) config.PowerLimiter_InverterChannelId, FLD_PAC);
    float dcVoltage = inverter->Statistics()->getChannelFieldValue(TYPE_DC, (ChannelNum_t) config.PowerLimiter_InverterChannelId, FLD_UDC); 

    if (dcVoltage <= 0.0) {
        return 0.0;
    }

    return dcVoltage + (acPower * config.PowerLimiter_VoltageLoadCorrectionFactor);
}

bool PowerLimiterClass::isStartThresholdReached(std::shared_ptr<InverterAbstract> inverter)
{
    CONFIG_T& config = Configuration.get();

    // Check if the Battery interface is enabled and the SOC start threshold is reached
    if (config.Battery_Enabled
            && config.PowerLimiter_BatterySocStartThreshold > 0.0
            && (millis() - Battery.stateOfChargeLastUpdate) < 60000
            && Battery.stateOfCharge >= config.PowerLimiter_BatterySocStartThreshold) {
        return true;
    }

    // Otherwise we use the voltage threshold
    if (config.PowerLimiter_VoltageStartThreshold <= 0.0) {
        return false;
    }

    float correctedDcVoltage = getLoadCorrectedVoltage(inverter);
    return correctedDcVoltage >= config.PowerLimiter_VoltageStartThreshold;
}

bool PowerLimiterClass::isStopThresholdReached(std::shared_ptr<InverterAbstract> inverter)
{
    CONFIG_T& config = Configuration.get();

    // Check if the Battery interface is enabled and the SOC stop threshold is reached
    if (config.Battery_Enabled
            && config.PowerLimiter_BatterySocStopThreshold > 0.0
            && (millis() - Battery.stateOfChargeLastUpdate) < 60000
            && Battery.stateOfCharge <= config.PowerLimiter_BatterySocStopThreshold) {
        return true;
    }

    // Otherwise we use the voltage threshold
    if (config.PowerLimiter_VoltageStopThreshold <= 0.0) {
        return false;
    }

    float correctedDcVoltage = getLoadCorrectedVoltage(inverter);
    return correctedDcVoltage <= config.PowerLimiter_VoltageStopThreshold;
}
