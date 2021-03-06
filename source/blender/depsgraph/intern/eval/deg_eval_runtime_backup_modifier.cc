/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2019 Blender Foundation.
 * All rights reserved.
 */

/** \file
 * \ingroup depsgraph
 */

#include "intern/eval/deg_eval_runtime_backup_modifier.h"

namespace blender {
namespace deg {

ModifierDataBackupID::ModifierDataBackupID(const Depsgraph * /*depsgraph*/)
    : ModifierDataBackupID(nullptr, eModifierType_None)
{
}

ModifierDataBackupID::ModifierDataBackupID(ModifierData *modifier_data, ModifierType type)
    : modifier_data(modifier_data), type(type)
{
}

bool operator==(const ModifierDataBackupID &a, const ModifierDataBackupID &b)
{
  return a.modifier_data == b.modifier_data && a.type == b.type;
}

uint64_t ModifierDataBackupID::hash() const
{
  uintptr_t ptr = (uintptr_t)modifier_data;
  return (ptr >> 4) ^ (uintptr_t)type;
}

}  // namespace deg
}  // namespace blender
