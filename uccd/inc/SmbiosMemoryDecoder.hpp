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

#include "SystemInfo.hpp"

#include <vector>

/**
 * @brief Decode SMBIOS Type 17 (Memory Device) entries directly from
 * /sys/firmware/dmi/tables/DMI.
 */
[[nodiscard]] std::vector< MemoryModuleInfo > detectMemoryModulesFromSmbios();
