package com.example.ble_beacon

import android.content.Context
import org.json.JSONObject
import java.util.zip.ZipInputStream

/**
 * The two dongle PCBs. Each has its own firmware build; [board] is the Zephyr
 * board name that build reports (manifest "board" and the dongle's board
 * characteristic), used to refuse an image meant for the other PCB.
 */
enum class Pcb(val label: String, val asset: String, val board: String) {
    OFFICIAL("Official Nordic dongle (PCA10059)", "firmware/nrf52840dongle.zip", "nrf52840dongle"),
    MDBT50Q("Raytac MDBT50Q-RX dongle", "firmware/mdbt50q.zip", "nrf52840dk"),
}

/** A signed MCUboot application image, ready to upload over SMP. */
class FirmwareImage(val pcb: Pcb, val version: String, val image: ByteArray)

/**
 * Firmware packages bundled into the APK at build time (see the
 * bundleFirmware task in app/build.gradle.kts). Each asset is the NCS
 * dfu_application.zip: manifest.json + the signed .bin it names.
 */
object FirmwareBundle {

    /** Bundled image for [pcb], or null if this APK was built without it. */
    fun load(ctx: Context, pcb: Pcb): FirmwareImage? {
        val entries = try {
            ctx.assets.open(pcb.asset).use { input ->
                ZipInputStream(input).use { zip ->
                    generateSequence { zip.nextEntry }
                        .associate { it.name to zip.readBytes() }
                }
            }
        } catch (_: java.io.IOException) {
            return null
        }

        val manifest = entries["manifest.json"] ?: return null
        val file = JSONObject(String(manifest, Charsets.UTF_8))
            .getJSONArray("files").getJSONObject(0)
        // Belt and braces: the build bundles by path, so make sure the zip in
        // this slot really was built for this PCB.
        if (file.optString("board") != pcb.board) return null
        val image = entries[file.getString("file")] ?: return null
        return FirmwareImage(pcb, file.optString("version_MCUBOOT", "?"), image)
    }
}
