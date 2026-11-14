#!/usr/bin/env bash
# Tests for the root helper, run without root against fake sysfs / power_supply /
# auto-cpufreq trees. The helper must be built with -DPRUEBA -DHW_RAIZ="<root>".
#
#   tests/ayudante.sh <helper-binary> <root-dir>
set -u

AYU=$1
R=$2
case "$R" in */prueba/raiz) ;; *) echo "refusing to use root dir '$R'"; exit 2 ;; esac

pasan=0 fallan=0
ok() { # ok "description" command...
    if "${@:2}"; then pasan=$((pasan + 1)); else fallan=$((fallan + 1)); echo "  FAIL: $1"; fi
}
seccion() { # seccion <name>: prints the lines of [name] in the auto-cpufreq config
    awk -v s="[$1]" '/^\[/{en=($0==s); next} en' "$R/etc/auto-cpufreq.conf"
}
tiene() { seccion "$1" | grep -qxF -- "$2"; }
no_tiene() { ! seccion "$1" | grep -qF -- "$2"; }
max_es() { for p in "$R"/sys/devices/system/cpu/cpufreq/policy*; do [ "$(cat "$p/scaling_max_freq")" = "$1" ] || return 1; done; }
vale() { [ "$(cat "$R/$1")" = "$2" ]; }
corre() { "$AYU" "$@" >/dev/null 2>&1; echo $?; }

# ---------------------------------------------------------------- fake trees
nuevo() { rm -rf "$R"; mkdir -p "$R"; }

cpu_acpi() { # 2 policies, discrete list, cpufreq/boost
    for i in 0 1; do
        local p="$R/sys/devices/system/cpu/cpufreq/policy$i"; mkdir -p "$p"
        echo 1400000 > "$p/cpuinfo_min_freq"; echo 2600000 > "$p/cpuinfo_max_freq"
        echo "2600000 1700000 1400000 " > "$p/scaling_available_frequencies"
        echo 2600000 > "$p/scaling_max_freq"; echo acpi-cpufreq > "$p/scaling_driver"
    done
    echo 1 > "$R/sys/devices/system/cpu/cpufreq/boost"
}

cpu_intel() { # continuous range, base_frequency, inverted no_turbo
    for i in 0 1; do
        local p="$R/sys/devices/system/cpu/cpufreq/policy$i"; mkdir -p "$p"
        echo 800000 > "$p/cpuinfo_min_freq"; echo 3900000 > "$p/cpuinfo_max_freq"
        echo 2100000 > "$p/base_frequency"; echo 3900000 > "$p/scaling_max_freq"
        echo intel_pstate > "$p/scaling_driver"
    done
    mkdir -p "$R/sys/devices/system/cpu/intel_pstate"
    echo 0 > "$R/sys/devices/system/cpu/intel_pstate/no_turbo"
}

energia() { # energia charger|battery
    local ps="$R/sys/class/power_supply"; mkdir -p "$ps/ADP1" "$ps/BAT0"
    echo Mains > "$ps/ADP1/type"; echo Battery > "$ps/BAT0/type"
    if [ "$1" = charger ]; then echo 1 > "$ps/ADP1/online"; echo Charging > "$ps/BAT0/status"
    else echo 0 > "$ps/ADP1/online"; echo Discharging > "$ps/BAT0/status"; fi
}

acf() { # auto-cpufreq installed with the default-style config
    mkdir -p "$R/etc" "$R/opt/auto-cpufreq"
    cat > "$R/etc/auto-cpufreq.conf" <<'EOF'
# Settings when connected to a charger
[charger]
governor = performance
turbo = auto

# Settings when running on battery
[battery]
governor = powersave
turbo = never
EOF
    cp "$R/etc/auto-cpufreq.conf" "$R/original.conf"
}

# ---------------------------------------------------------------- tests
echo "== whitelist"
nuevo; cpu_acpi; energia charger; acf
for a in "" "1.7" "/etc/passwd" "../../etc/shadow" "1800000" "99999999" "abc" "-1" "1400000x" "0001400000"; do
    ok "rejects '$a' with code 2" [ "$(corre "$a")" = 2 ]
done
ok "rejects no arguments" [ "$(corre)" = 2 ]
ok "rejects two arguments" [ "$(corre 1400000 turbo)" = 2 ]
ok "rejected arguments leave sysfs untouched" max_es 2600000
ok "rejected arguments leave the config untouched" cmp -s "$R/etc/auto-cpufreq.conf" "$R/original.conf"

echo "== acpi-cpufreq + auto-cpufreq, on charger"
nuevo; cpu_acpi; energia charger; acf
ok "1400000 succeeds" [ "$(corre 1400000)" = 0 ]
ok "  limit written to every policy" max_es 1400000
ok "  turbo off" vale sys/devices/system/cpu/cpufreq/boost 0
ok "  marker in [charger]" tiene charger "# cpu-freq: 1400000"
ok "  turbo = never in [charger]" tiene charger "turbo = never"
ok "  scaling_max_freq in [charger]" tiene charger "scaling_max_freq = 1400000"
ok "  old turbo line replaced, not duplicated" [ "$(seccion charger | grep -c '^turbo')" = 1 ]
ok "  [battery] untouched" [ "$(seccion battery)" = "$(awk '/^\[/{en=($0=="[battery]");next} en' "$R/original.conf")" ]
ok "  backup equals the original" cmp -s "$R/etc/auto-cpufreq.conf.cpu-freq-orig" "$R/original.conf"
ok "1700000 succeeds" [ "$(corre 1700000)" = 0 ]
ok "  only one marker" [ "$(seccion charger | grep -c 'cpu-freq:')" = 1 ]
ok "  backup still the original" cmp -s "$R/etc/auto-cpufreq.conf.cpu-freq-orig" "$R/original.conf"
ok "turbo succeeds" [ "$(corre turbo)" = 0 ]
ok "  turbo on" vale sys/devices/system/cpu/cpufreq/boost 1
ok "  limit back to the hardware max" max_es 2600000
ok "  turbo = always" tiene charger "turbo = always"
ok "  no scaling_max_freq with turbo" no_tiene charger "scaling_max_freq"
ok "auto succeeds" [ "$(corre auto)" = 0 ]
ok "  marker removed" no_tiene charger "cpu-freq:"
ok "  original turbo = auto restored" tiene charger "turbo = auto"
ok "  no scaling_max_freq left" no_tiene charger "scaling_max_freq"
ok "  governor kept" tiene charger "governor = performance"
ok "  no temp file left behind" [ ! -e "$R/etc/.auto-cpufreq.conf.cpu-freq" ]

echo "== acpi-cpufreq + auto-cpufreq, on battery"
nuevo; cpu_acpi; energia battery; acf
ok "1700000 succeeds" [ "$(corre 1700000)" = 0 ]
ok "  marker goes to [battery]" tiene battery "# cpu-freq: 1700000"
ok "  [charger] untouched" no_tiene charger "cpu-freq:"
ok "auto succeeds" [ "$(corre auto)" = 0 ]
ok "  turbo = never back in [battery]" tiene battery "turbo = never"
ok "  marker removed from [battery]" no_tiene battery "cpu-freq:"

echo "== no battery at all (desktop) counts as charger"
nuevo; cpu_acpi; acf; mkdir -p "$R/sys/class/power_supply"
ok "1400000 succeeds" [ "$(corre 1400000)" = 0 ]
ok "  marker goes to [charger]" tiene charger "# cpu-freq: 1400000"

echo "== without auto-cpufreq"
nuevo; cpu_acpi; energia charger
ok "1700000 succeeds" [ "$(corre 1700000)" = 0 ]
ok "  limit written" max_es 1700000
ok "  turbo off" vale sys/devices/system/cpu/cpufreq/boost 0
ok "  no config created" [ ! -e "$R/etc/auto-cpufreq.conf" ]
ok "auto succeeds" [ "$(corre auto)" = 0 ]
ok "  limit back to the hardware max" max_es 2600000
ok "  turbo on" vale sys/devices/system/cpu/cpufreq/boost 1

echo "== auto-cpufreq installed without a config file"
nuevo; cpu_acpi; energia charger; mkdir -p "$R/opt/auto-cpufreq" "$R/etc"
ok "1400000 succeeds" [ "$(corre 1400000)" = 0 ]
ok "  config created with [charger]" tiene charger "scaling_max_freq = 1400000"
ok "  empty backup (there was no file)" [ -e "$R/etc/auto-cpufreq.conf.cpu-freq-orig" ] && [ ! -s "$R/etc/auto-cpufreq.conf.cpu-freq-orig" ]
ok "auto succeeds" [ "$(corre auto)" = 0 ]
ok "  keys removed again" no_tiene charger "scaling_max_freq"

echo "== intel_pstate (continuous range, inverted no_turbo)"
nuevo; cpu_intel; energia charger
ok "2000000 inside the range succeeds" [ "$(corre 2000000)" = 0 ]
ok "  limit written" max_es 2000000
ok "  no_turbo = 1 (turbo off)" vale sys/devices/system/cpu/intel_pstate/no_turbo 1
ok "rejects 3950000 above the max" [ "$(corre 3950000)" = 2 ]
ok "rejects 700000 below the min" [ "$(corre 700000)" = 2 ]
ok "rejects 2000500 (not whole MHz)" [ "$(corre 2000500)" = 2 ]
ok "turbo succeeds" [ "$(corre turbo)" = 0 ]
ok "  no_turbo = 0 (turbo on)" vale sys/devices/system/cpu/intel_pstate/no_turbo 0
ok "  limit back to the hardware max" max_es 3900000

echo "== no cpufreq at all"
nuevo; energia charger
ok "fails with code 1" [ "$(corre 1400000)" = 1 ]

rm -rf "$R"
echo
echo "$pasan passed, $fallan failed"
[ "$fallan" = 0 ]
