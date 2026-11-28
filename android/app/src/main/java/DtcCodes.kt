package com.oliver.m3cflasher

/**
 * Fault code descriptions (English), same list as the desktop diagnostics
 * GUI (dtc_codes.py). The blink code is the most likely match from the
 * owner's manual fault table - not confirmed by KTM for every code.
 * Blink format: "1L 2S" = 1 long + 2 short flashes = code 12.
 */
object DtcCodes {
    data class Info(val description: String, val blink: String)

    private val CODES: Map<String, Info> = mapOf(
        "P0335" to Info("Crankshaft position sensor - circuit fault / no signal", "02 (2S)"),
        "P0336" to Info("Crankshaft position sensor - signal implausible / irregular", "02 (2S)"),
        "P0337" to Info("Crankshaft position sensor - signal too low", "02 (2S)"),
        "P0338" to Info("Crankshaft position sensor - signal too high", "02 (2S)"),
        "P0339" to Info("Crankshaft position sensor - signal intermittent", "02 (2S)"),
        "P0016" to Info("Crankshaft position - correlation / synchronization faulty", "02 (2S)"),
        "P0120" to Info("Throttle position sensor A - circuit fault", "06 (6S)"),
        "P0121" to Info("Throttle position sensor A - range/performance (adaption failed)", "06 (6S)"),
        "P0122" to Info("Throttle position sensor A - input signal too low", "06 (6S)"),
        "P0123" to Info("Throttle position sensor A - input signal too high", "06 (6S)"),
        "P0638" to Info("Throttle actuator control - range/performance", "06 (6S)"),
        "P0105" to Info("Crankcase / manifold pressure sensor - circuit fault", "09 (9S)"),
        "P0106" to Info("Crankcase / manifold pressure sensor - range/performance, difference to ECU value too high", "14 (1L 4S)"),
        "P0107" to Info("Crankcase / manifold pressure sensor - short circuit to ground (low)", "09 (9S)"),
        "P0108" to Info("Crankcase / manifold pressure sensor - open / short circuit to plus (high)", "09 (9S)"),
        "P0101" to Info("Air flow / air mass calculation - range/performance", ""),
        "P0102" to Info("Air flow / air mass calculation - input too low", ""),
        "P0103" to Info("Air flow / air mass calculation - input too high", ""),
        "P2227" to Info("Ambient air pressure sensor - range/performance", "09 (9S)"),
        "P2228" to Info("Ambient air pressure sensor - short circuit to ground (low)", "09 (9S)"),
        "P2229" to Info("Ambient air pressure sensor - open / short circuit to plus (high)", "09 (9S)"),
        "P2230" to Info("Ambient air pressure sensor - signal intermittent", "09 (9S)"),
        "P0069" to Info("Manifold pressure vs. ambient pressure - correlation", ""),
        "P0070" to Info("Ambient air temperature sensor - circuit fault", ""),
        "P0071" to Info("Ambient air temperature sensor - range/performance", ""),
        "P0072" to Info("Ambient air temperature sensor - input signal too low", ""),
        "P0073" to Info("Ambient air temperature sensor - input signal too high", ""),
        "P0110" to Info("Intake air temperature sensor - circuit fault", "13 (1L 3S)"),
        "P0111" to Info("Intake air temperature sensor - range/performance", "13 (1L 3S)"),
        "P0112" to Info("Intake air temperature sensor - input signal too low", "13 (1L 3S)"),
        "P0113" to Info("Intake air temperature sensor - input signal too high", "13 (1L 3S)"),
        "P0115" to Info("Coolant temperature sensor - circuit fault", "12 (1L 2S)"),
        "P0116" to Info("Coolant temperature sensor - range/performance", "12 (1L 2S)"),
        "P0117" to Info("Coolant temperature sensor - input signal too low", "12 (1L 2S)"),
        "P0118" to Info("Coolant temperature sensor - input signal too high", "12 (1L 2S)"),
        "P0125" to Info("Coolant temperature too low for closed loop control", ""),
        "P0217" to Info("Engine overtemperature", ""),
        "P0560" to Info("System (battery) voltage - circuit fault", "21 (2L 1S)"),
        "P0562" to Info("Battery voltage - input voltage too low", "21 (2L 1S)"),
        "P0563" to Info("Battery voltage - input voltage too high", "21 (2L 1S)"),
        "P0200" to Info("Injector circuit - fault", ""),
        "P0201" to Info("Injection valve 0, cylinder 1 (low load injector) - circuit fault", "33 (3L 3S)"),
        "P0261" to Info("Injection valve 0, cylinder 1 - input signal too low", "33 (3L 3S)"),
        "P0262" to Info("Injection valve 0, cylinder 1 - input signal too high", "33 (3L 3S)"),
        "P0202" to Info("Injector 2 circuit - likely injection valve 1 (top feed injector)", "34 (3L 4S)"),
        "P0264" to Info("Injector 2 circuit low - likely injection valve 1 (top feed injector)", "34 (3L 4S)"),
        "P0265" to Info("Injector 2 circuit high - likely injection valve 1 (top feed injector)", "34 (3L 4S)"),
        "P0351" to Info("Ignition coil - circuit fault", "37 (3L 7S)"),
        "P2300" to Info("Ignition coil - primary circuit low", "37 (3L 7S)"),
        "P2301" to Info("Ignition coil - primary circuit high", "37 (3L 7S)"),
        "P0230" to Info("Fuel pump - primary circuit fault", "41 (4L 1S)"),
        "P0231" to Info("Fuel pump - short circuit to ground / open circuit (low)", "41 (4L 1S)"),
        "P0232" to Info("Fuel pump - open circuit / short circuit to plus (high)", "41 (4L 1S)"),
        "P0627" to Info("Fuel pump control - circuit open", "41 (4L 1S)"),
        "P0628" to Info("Fuel pump control - circuit low", "41 (4L 1S)"),
        "P0629" to Info("Fuel pump control - circuit high", "41 (4L 1S)"),
        "P206C" to Info("Oil pump circuit (reported on KTM group 2-strokes)", "42 (4L 2S)"),
        "P260C" to Info("Oil pump circuit (reported on KTM group 2-strokes)", "42 (4L 2S)"),
        "P0505" to Info("Idle air control system - fault", ""),
        "P0506" to Info("Idle speed lower than expected", ""),
        "P0507" to Info("Idle speed higher than expected", ""),
        "P0500" to Info("Vehicle speed sensor - fault", ""),
        "P0600" to Info("ECU communication (serial link) - fault", ""),
        "P0601" to Info("ECU internal memory checksum error", ""),
        "P0603" to Info("ECU internal keep-alive memory error", ""),
        "P0604" to Info("ECU internal RAM error", ""),
        "P0605" to Info("ECU internal ROM error", ""),
        "P0606" to Info("ECU processor fault", ""),
        "P0650" to Info("Malfunction indicator lamp (MIL) - circuit fault", ""),
        "U0001" to Info("CAN bus communication - fault", ""),
        "U0100" to Info("Lost communication with engine control unit", ""),
    )

    private val GROUPS = listOf(
        "P00" to "Fuel / air metering and auxiliary emission controls",
        "P01" to "Fuel / air metering",
        "P02" to "Fuel / air metering (injector circuit)",
        "P03" to "Ignition system or misfire",
        "P04" to "Auxiliary emission controls",
        "P05" to "Vehicle speed, idle control and auxiliary inputs",
        "P06" to "ECU and auxiliary outputs",
        "P07" to "Transmission",
        "P1" to "Manufacturer specific (KTM / Continental)",
        "P2" to "Fuel / air metering, auxiliary controls",
        "P3" to "Manufacturer specific / reserved",
        "U" to "Network / CAN communication",
        "C" to "Chassis",
        "B" to "Body",
    )

    /** All known codes, sorted - for the code list / search. */
    val all: List<Pair<String, Info>> get() = CODES.toList().sortedBy { it.first }

    fun describe(code: String): Info {
        val c = code.trim().uppercase()
        CODES[c]?.let { return it }
        GROUPS.firstOrNull { c.startsWith(it.first) }?.let { return Info("Unknown code - group: ${it.second}", "") }
        return Info("Unknown code", "")
    }
}
