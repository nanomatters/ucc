/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

namespace ucc
{

/**
 * @brief Water cooler pump voltage levels
 */
enum class PumpVoltage {
    V11 = 0x00,
    V12 = 0x01,
    V7 = 0x02,
    V8 = 0x03,
    Off = 0x04
};

/**
 * @brief Liquid Cooled Technology (LCT) water cooler device models
 */
enum class LCTDeviceModel {
    Unknown = -1,
    LCT21001 = 0,
    LCT22002 = 1
};

/**
 * @brief RGB state/mode for LCT water cooler devices
 */
enum class RGBState {
    Static = 0x00,
    Breathe = 0x01,
    Colorful = 0x02,
    BreatheColor = 0x03,
    Temperature = 0x04       // GUI-only mode: daemon maps to Static + auto-color
};

} // namespace ucc