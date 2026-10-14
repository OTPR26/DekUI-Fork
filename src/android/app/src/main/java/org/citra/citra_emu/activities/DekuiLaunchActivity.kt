// SPDX-License-Identifier: GPL-2.0-or-later
package org.citra.citra_emu.activities

import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.widget.Button
import android.widget.LinearLayout
import android.widget.TextView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import org.citra.citra_emu.NativeLibrary
import org.citra.citra_emu.utils.DekuiGameSetup
import org.citra.citra_emu.utils.DirectoryInitialization

/** Presentation startup: one game selection, no emulator setup wizard. */
class DekuiLaunchActivity : AppCompatActivity() {
    private lateinit var status: TextView
    private lateinit var select: Button
    private val prefs by lazy { getSharedPreferences("dekui-launch", MODE_PRIVATE) }
    private val game by lazy { File(filesDir, "dekui-game/game.cci") }
    private val picker = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) importGame(uri)
    }
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val layout = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = android.view.Gravity.CENTER
            val padding = (24 * resources.displayMetrics.density).toInt()
            setPadding(padding,padding,padding,padding)
        }
        status = TextView(this).apply {
            text = "Select your decrypted Majora’s Mask 3D USA Rev 1 game. It will be imported once; future launches open it automatically."
            textSize = 18f
        }
        select = Button(this).apply {
            text = "Select game"
            setOnClickListener { picker.launch(arrayOf("*/*")) }
        }
        layout.addView(status); layout.addView(select); setContentView(layout)
        if (game.isFile && prefs.getBoolean("ready",false)) openGame()
    }
    private fun importGame(uri: Uri) {
        select.isEnabled = false
        status.text = "Preparing your game…"
        lifecycleScope.launch {
            val result = withContext(Dispatchers.IO) {
                runCatching {
                    check(DirectoryInitialization.areCitraDirectoriesReady()) { "App storage could not be initialized" }
                    val parent = game.parentFile!!; check(parent.isDirectory || parent.mkdirs())
                    val stage = File(parent,"import-${java.util.UUID.randomUUID()}.cci")
                    try {
                        contentResolver.openInputStream(uri)?.use { input ->
                            stage.outputStream().use { output -> input.copyTo(output) }
                        } ?: error("The selected file could not be opened")
                        DekuiGameSetup.prepare(this@DekuiLaunchActivity, stage, File(NativeLibrary.getUserDirectory()))
                        check(!game.exists()) { "A game is already installed" }
                        check(stage.renameTo(game)) { "Could not finish importing the game" }
                        check(prefs.edit().putBoolean("ready",true).commit())
                    } finally { stage.delete() }
                }
            }
            result.fold({ openGame() }, {
                status.text = it.message ?: "Game preparation failed. Please select the supported game file."
                select.isEnabled = true
            })
        }
    }
    private fun openGame() {
        startActivity(Intent(this,EmulationActivity::class.java).apply {
            action = Intent.ACTION_VIEW
            data = Uri.fromFile(game)
            putExtra("launched_from_shortcut",true)
        })
        finish()
    }
}
