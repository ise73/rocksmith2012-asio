#!/bin/bash
#
# Launches Rocksmith or Rocksmith 2014 through Steam/Proton with the
# environment PipeASIO needs. Edit the "USER CONFIGURATION" section below
# to match your system, then run this script instead of launching the
# game directly from Steam.

set -euo pipefail

# ============================================================
#  USER CONFIGURATION — edit the values in this section only
# ============================================================

# Which game to launch: 'Rocksmith' or 'Rocksmith2014'
GAME='Rocksmith'

# PipeWire (and PipeASIO) quantum/sample rate for the game's audio. Lower values reduce
# latency but may cause dropouts; see the setup guide for tuning tips.
#
# NOTE: if PipeWire has a daemon-wide forced quantum/rate (checked and pinned
# below via pw-metadata), it overrides any per-app request - including
# PipeASIO's own. These two values are also used below to pin PipeWire's
# daemon-wide clock so they actually take effect.
export PIPEASIO_PREFERRED_BUFFERSIZE='128'
export PIPEASIO_SAMPLE_RATE='48000'

# Not needed for PipeASIO - kept for reference. To be removed in the future.
# export PIPEWIRE_LATENCY="${PIPEASIO_PREFERRED_BUFFERSIZE}/${PIPEASIO_SAMPLE_RATE}"

# LACT (Linux AMDGPU Controller) profile configurations.
# Set LACT_GAME_PROFILE to your preferred overclock/undervolt profile for playing.
# Set LACT_IDLE_PROFILE to the profile you want to return to when closing the game.
LACT_GAME_PROFILE='game'
LACT_IDLE_PROFILE='idle'

# Steam's default library, where Steam itself and its compat tools are
# installed. This is not necessarily where the game itself lives.
STEAM_HOME="${HOME}/.local/share/Steam"

# Steam Library where the game is installed. Change this if you use a
# separate library on another disk/mount than STEAM_HOME.
STEAMAPPS="${STEAM_HOME}/steamapps"

# Path to the Proton build to use. Pre-set for Proton-CachyOS.
# Other probable locations:
#   "${STEAMAPPS}/common/Proton 11.0/proton"
#   "${STEAM_HOME}/compatibilitytools.d/GE-Proton<VERSION>/proton"
PROTON_BIN='/usr/share/steam/compatibilitytools.d/proton-cachyos-slr/proton'

# Steam Linux Runtime to launch through. Both of these are known to work;
# use whichever one is installed on your system:
#   'SteamLinuxRuntime_4'
#   'SteamLinuxRuntime_sniper'
STEAM_RUNTIME_LAUNCHER="${STEAMAPPS}/common/SteamLinuxRuntime_4/pressure-vessel/bin/steam-runtime-launcher-interface-0"

# Required by PipeASIO — should not normally need changing.
export PROTON_USE_WOW64='1'
export WINEDLLPATH="${HOME}/.local/lib/wine"

# ============================================================
#  INTERNAL — derived automatically, no need to edit below this line
# ============================================================

GAME_DIR="${STEAMAPPS}/common/${GAME}"

# Steam's app ID for the selected game
if [[ "${GAME}" = 'Rocksmith' ]]; then
    APP='205190'
elif [[ "${GAME}" = 'Rocksmith2014' ]]; then
    APP='221680'
else
    echo 'Error: set the GAME variable to "Rocksmith" or "Rocksmith2014".'
    exit 1
fi

# Identify the game to Steam/Proton
export SteamAppId="${APP}"
export SteamGameId="${APP}"
export STEAM_COMPAT_APP_ID="${APP}"
export SteamOverlayGameId="${APP}"
export STEAM_COMPAT_CLIENT_INSTALL_PATH="${STEAM_HOME}"
export STEAM_COMPAT_SHADER_PATH="${STEAMAPPS}/shadercache/${APP}"
export STEAM_COMPAT_INSTALL_PATH="${GAME_DIR}"
export STEAM_COMPAT_DATA_PATH="${STEAMAPPS}/compatdata/${APP}"
export SteamClientLaunch='1'
export SteamEnv='1'

# ============================================================
#  RESTORE SYSTEM SETTINGS
# ============================================================
ORIG_FORCE_QUANTUM=''
ORIG_FORCE_RATE=''
LACT_PROFILE_CHANGED=false

restore_system_settings() {
    echo "Cleaning up and restoring system profiles..."

    # 1. Restore PipeWire Clock Settings
    if command -v pw-metadata >/dev/null; then
        if [[ -n "${ORIG_FORCE_QUANTUM}" ]]; then
            pw-metadata -n settings 0 clock.force-quantum "${ORIG_FORCE_QUANTUM}" >/dev/null || true
        else
            pw-metadata -n settings -d 0 clock.force-quantum >/dev/null || true
        fi

        if [[ -n "${ORIG_FORCE_RATE}" ]]; then
            pw-metadata -n settings 0 clock.force-rate "${ORIG_FORCE_RATE}" >/dev/null || true
        else
            pw-metadata -n settings -d 0 clock.force-rate >/dev/null || true
        fi
    fi

    # 2. Restore LACT GPU Overclocking Profile
    if [[ "${LACT_PROFILE_CHANGED}" = true ]] && command -v lact >/dev/null; then
        lact cli profile set "${LACT_IDLE_PROFILE}" >/dev/null
        echo "LACT overclocking profile ${LACT_IDLE_PROFILE} set"
    fi
}

# Single unified trap for all exits/signals
trap restore_system_settings EXIT

# ============================================================
#  APPLY SYSTEM SETTINGS
# ============================================================

# Set up PipeWire pinning
if command -v pw-metadata >/dev/null; then
    # Improved sed regex: matches value:'123', value:"123", or value:123
    ORIG_FORCE_QUANTUM=$(pw-metadata -n settings 0 clock.force-quantum 2>/dev/null | sed -n "s/.*value:['\"]*\([^'\"]*\)['\"]*.*/\1/p")
    ORIG_FORCE_RATE=$(pw-metadata -n settings 0 clock.force-rate 2>/dev/null | sed -n "s/.*value:['\"]*\([^'\"]*\)['\"]*.*/\1/p")

    # If the current value is explicitly "0" (auto), treat it as an empty/unset state
    [[ "${ORIG_FORCE_QUANTUM}" == "0" ]] && ORIG_FORCE_QUANTUM=""
    [[ "${ORIG_FORCE_RATE}" == "0" ]] && ORIG_FORCE_RATE=""

    pw-metadata -n settings 0 clock.force-quantum "${PIPEASIO_PREFERRED_BUFFERSIZE}" >/dev/null
    pw-metadata -n settings 0 clock.force-rate "${PIPEASIO_SAMPLE_RATE}" >/dev/null
else
    >&2 echo 'Warning: pw-metadata not found; cannot pin PipeWire'\''s daemon-wide quantum/rate.'
fi

# Set up LACT GPU profile
if command -v lact >/dev/null; then
    lact cli profile set "${LACT_GAME_PROFILE}" >/dev/null
    echo "LACT overclocking profile ${LACT_GAME_PROFILE} set"
    LACT_PROFILE_CHANGED=true
fi

# ============================================================
#  LAUNCH THE GAME
# ============================================================

LAUNCH_CMD=(
    env --chdir="${GAME_DIR}" \
    "${STEAM_RUNTIME_LAUNCHER}" \
    container-runtime "${PROTON_BIN}" \
    waitforexitandrun "${GAME_DIR}/${GAME}.exe" -uplay_steam_mode
)

# game-performance (CachyOS) must wrap the whole chain above, not just the
# exe, so it can restore the power profile once everything under it exits.
if command -v game-performance >/dev/null; then
    LAUNCH_CMD=(game-performance "${LAUNCH_CMD[@]}")
fi

# Execute the final built command array
"${LAUNCH_CMD[@]}"
