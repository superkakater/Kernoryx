#!/usr/bin/env python3
from pathlib import Path
import sys

def create(root):
    root = Path(root)
    values = {
        'firmware/acpi/platform_profile': 'balanced',
        'firmware/acpi/platform_profile_choices': 'low-power balanced performance',
        'devices/system/cpu/intel_pstate/status': 'active',
        'devices/system/cpu/intel_pstate/no_turbo': '0',
        'class/hwmon/hwmon0/name': 'coretemp',
        'class/hwmon/hwmon0/temp1_input': '55000',
        'class/power_supply/AC/type': 'Mains',
        'class/power_supply/AC/online': '1',
    }
    for i in (0, 1):
        prefix = f'devices/system/cpu/cpufreq/policy{i}/'
        values.update({prefix + k: v for k, v in {
            'scaling_driver': 'intel_pstate',
            'scaling_governor': 'powersave',
            'scaling_available_governors': 'powersave performance',
            'energy_performance_preference': 'balance_power',
            'energy_performance_available_preferences': 'default power balance_power balance_performance performance',
        }.items()})
    for name, value in values.items():
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(value + '\n')
    return root

if __name__ == '__main__':
    create(sys.argv[1])
