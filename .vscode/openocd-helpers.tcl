#
# Custom openocd-helpers for Cortex-Debug
# Overrides CDLiveWatchSetup to avoid compatibility issues with OpenOCD 0.12.0
#
set USE_SWO 0
proc CDSWOConfigure { CDCPUFreqHz CDSWOFreqHz CDSWOOutput } {
    catch {tpiu init}
    set tipu_names [tpiu names]
    if { [llength $tipu_names] == 0 } {
        puts stderr "[info script]: Error: Could not find TPIU/SWO names."
    } else {
        set mytpiu [lindex $tipu_names 0]
        $mytpiu configure -protocol uart -output $CDSWOOutput -traceclk $CDCPUFreqHz -pin-freq $CDSWOFreqHz
        $mytpiu enable
    }
}

proc CDRTOSConfigure { rtos } {
    set target [target current]
    if { $target != "" } {
        $target configure -rtos $rtos
    } else {
        puts stderr "[info script]: Error: No current target. Could not configure target for RTOS"
    }
}

# Override to avoid issues with gdb-max-connections on OpenOCD 0.12.0
proc CDLiveWatchSetup {} {
    puts "CDLiveWatchSetup: using custom no-op implementation"
}