#!/usr/bin/env bash
set -euo pipefail
package_name="io.github.tgacquin.dekui.experiment.debug"
activity_name="org.citra.citra_emu.ui.main.MainActivity"
adb logcat -c
adb shell am start -n "$package_name/$activity_name" >/dev/null
sleep 3
adb logcat -d -v time -t 1200 | grep -E 'FATAL|AndroidRuntime|SIGSEGV|SIGABRT|CitraNative|Dekui|Qt|Vulkan|Exception' | tail -160 || true
