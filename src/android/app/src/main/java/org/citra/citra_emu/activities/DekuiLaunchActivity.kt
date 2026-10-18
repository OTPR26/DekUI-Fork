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
import org.citra.citra_emu.utils.DekuiTextureStorage
import org.citra.citra_emu.utils.DekuiGameSetup
import org.citra.citra_emu.utils.DirectoryInitialization

/** Presentation startup: one game selection, no emulator setup wizard. */
class DekuiLaunchActivity : AppCompatActivity() {
    private lateinit var status: TextView
    private lateinit var select: Button
    private val prefs by lazy { getSharedPreferences("dekui-launch", MODE_PRIVATE) }
    private val game get() = org.citra.citra_emu.utils.DekuiStorage.game(this)
    private val picker = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        if (uri != null) importGame(uri)
    }
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        prepareSharedStorage()
        onBackPressedDispatcher.addCallback(this,object : androidx.activity.OnBackPressedCallback(true) {
            override fun handleOnBackPressed() { finish() }
        })
    }

    private val storagePermission = registerForActivityResult(ActivityResultContracts.StartActivityForResult()) {
        prepareSharedStorage()
    }
    private val legacyStoragePermission = registerForActivityResult(ActivityResultContracts.RequestPermission()) {
        prepareSharedStorage()
    }
    private var preparingStorage=false
    private fun prepareSharedStorage() {
        if(preparingStorage)return
        if(!org.citra.citra_emu.utils.DekuiStorage.hasAccess(this)) {
            val panel=LinearLayout(this).apply { orientation=LinearLayout.VERTICAL;setPadding(48,48,48,48) }
            panel.addView(TextView(this).apply { text="Game data folder\n\nDekUI stores saves, mods and settings in Internal storage/mm3dport. Allow file access to create this folder and migrate existing data. Your existing private data will be kept as a backup.";textSize=20f })
            panel.addView(Button(this).apply { text="Allow file access";setOnClickListener {
                if(android.os.Build.VERSION.SDK_INT>=30)storagePermission.launch(Intent(android.provider.Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,Uri.parse("package:$packageName")))
                else legacyStoragePermission.launch(android.Manifest.permission.WRITE_EXTERNAL_STORAGE)
            }})
            setContentView(panel);return
        }
        preparingStorage=true
        val message=TextView(this).apply {text="Preparing Internal storage/mm3dport…\nCopying and verifying existing data. Originals will be retained.";textSize=20f;setPadding(48,48,48,48)}
        setContentView(message)
        lifecycleScope.launch {
            val result=withContext(Dispatchers.IO){runCatching{org.citra.citra_emu.utils.DekuiStorage.migrate(this@DekuiLaunchActivity)}}
            preparingStorage=false
            result.fold({finishStorageSetup()},{message.text="Game data was retained.\n"+(it.message?:"Storage migration failed")})
        }
    }
    private fun finishStorageSetup() {
        if (!prefs.getBoolean("controller-defaults-v1", prefs.getBoolean("odin-controls-v1", false))) {
            if(!org.citra.citra_emu.utils.DekuiStorage.hasSharedBindings(this)) {
                org.citra.citra_emu.features.settings.model.view.InputBindingSetting.applyDekuiDefaultBindings()
            }
            org.citra.citra_emu.utils.EmulationMenuSettings.showOverlay = false
            prefs.edit().putBoolean("controller-defaults-v1", true).apply()
        }
        val recovered=!prefs.getBoolean("ready",false) &&
            org.citra.citra_emu.utils.DekuiStorageMigration.hasPreparedGame(org.citra.citra_emu.utils.DekuiStorage.root(this))
        if(recovered)check(prefs.edit().putBoolean("ready",true).commit())
        buildLauncher()
        if(recovered)status.text="Existing game data restored. Re-select any external texture folder in Settings → Graphics."
    }

    private fun buildLauncher() {
        val density=resources.displayMetrics.density
        fun dp(value: Int)=(value*density).toInt()
        val light=android.graphics.Color.rgb(242,242,242)
        val dim=android.graphics.Color.rgb(204,204,204)
        val purple=android.graphics.Color.rgb(185,125,242)
        val bold=androidx.core.content.res.ResourcesCompat.getFont(this,org.citra.citra_emu.R.font.dekui_sans_bold)
        val normal=androidx.core.content.res.ResourcesCompat.getFont(this,org.citra.citra_emu.R.font.dekui_sans_regular)
        androidx.core.view.WindowCompat.setDecorFitsSystemWindows(window,false)
        androidx.core.view.WindowInsetsControllerCompat(window,window.decorView).apply {
            hide(androidx.core.view.WindowInsetsCompat.Type.systemBars())
            systemBarsBehavior=androidx.core.view.WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }
        val frame=android.widget.FrameLayout(this)
        frame.addView(org.citra.citra_emu.utils.DekuiLauncherBackdrop(this),android.widget.FrameLayout.LayoutParams(-1,-1))
        val pane=LinearLayout(this).apply {
            orientation=LinearLayout.VERTICAL
            gravity=android.view.Gravity.END
            setPadding(dp(12),dp(16),dp(28),dp(12))
        }
        frame.addView(pane,android.widget.FrameLayout.LayoutParams(
            (resources.displayMetrics.widthPixels*.69f).toInt(),-1,android.view.Gravity.END))
        val titleLabel=TextView(this).apply {
            text="DekUI Fork";typeface=bold;textSize=16f;setTextColor(purple)
            gravity=android.view.Gravity.END;letterSpacing=.07f
            importantForAccessibility=android.view.View.IMPORTANT_FOR_ACCESSIBILITY_NO
        }
        val title=TextView(this).apply {
            text="Majora's Mask 3D";typeface=bold;textSize=34f;setTextColor(light)
            gravity=android.view.Gravity.END;letterSpacing=.03f
            maxLines=1
            setAutoSizeTextTypeUniformWithConfiguration(22,34,1,android.util.TypedValue.COMPLEX_UNIT_SP)
        }
        pane.addView(titleLabel,LinearLayout.LayoutParams(-1,dp(24)))
        pane.addView(title,LinearLayout.LayoutParams(-1,dp(52)))
        pane.addView(android.view.View(this),LinearLayout.LayoutParams(1,0,1f))
        fun menuButton(label: String, action: () -> Unit): Button {
            return Button(this).apply {
                text=label;isAllCaps=false;typeface=bold;textSize=17f
                isFocusable=true;isFocusableInTouchMode=true
                letterSpacing=.07f;gravity=android.view.Gravity.END or android.view.Gravity.CENTER_VERTICAL
                minHeight=0;minimumHeight=0;includeFontPadding=false
                setPadding(dp(12),0,dp(12),0)
                setTextColor(android.content.res.ColorStateList(
                    arrayOf(intArrayOf(android.R.attr.state_focused),intArrayOf(android.R.attr.state_pressed),intArrayOf()),
                    intArrayOf(purple,purple,dim)))
                val highlight=android.graphics.drawable.GradientDrawable(
                    android.graphics.drawable.GradientDrawable.Orientation.LEFT_RIGHT,
                    intArrayOf(android.graphics.Color.TRANSPARENT,0x33B97DF2)).apply { cornerRadius=dp(4).toFloat() }
                background=android.graphics.drawable.StateListDrawable().apply {
                    addState(intArrayOf(android.R.attr.state_focused),highlight)
                    addState(intArrayOf(android.R.attr.state_pressed),highlight.constantState!!.newDrawable())
                    addState(intArrayOf(),android.graphics.drawable.ColorDrawable(android.graphics.Color.TRANSPARENT))
                }
                setOnClickListener { action() }
                pane.addView(this,LinearLayout.LayoutParams(-1,dp(36)))
            }
        }
            select=menuButton(if(game.isFile&&prefs.getBoolean("ready",false)) "Start game" else "Select ROM") {
                if(game.isFile&&prefs.getBoolean("ready",false))openGame() else picker.launch(arrayOf("*/*"))
            }
            menuButton("Settings") { openSettings() }
            menuButton("About") {
                androidx.appcompat.app.AlertDialog.Builder(this)
                    .setTitle("DekUI for Android")
                    .setMessage("Majora’s Mask 3D\n\n"+
                        "Swipe from the left edge during play for DekUI settings.\n\n"+
                        ""+
                        org.citra.citra_emu.BuildConfig.VERSION_NAME)
                    .setPositiveButton("Close",null).show()
            }
            menuButton("Exit") { finishAndRemoveTask() }
        status=TextView(this).apply {
            typeface=normal;textSize=12f;setTextColor(dim);gravity=android.view.Gravity.END
            text=if(game.isFile&&prefs.getBoolean("ready",false)) "" else "Select your Majora’s Mask 3D USA Rev 1 ROM to begin."
            setPadding(0,dp(6),dp(12),0)
            accessibilityLiveRegion=android.view.View.ACCESSIBILITY_LIVE_REGION_POLITE
        }
        pane.addView(status,LinearLayout.LayoutParams(-1,dp(32)))
        val version=TextView(this).apply {
            text="DEKUI  ·  ANDROID";typeface=normal;textSize=10f;letterSpacing=.1f
            setTextColor(0xFF9993A5.toInt());setPadding(dp(12),0,0,dp(8))
        }
        frame.addView(version,android.widget.FrameLayout.LayoutParams(-2,-2,android.view.Gravity.BOTTOM or android.view.Gravity.START))
        setContentView(frame)
        select.requestFocus()
    }

    override fun onSaveInstanceState(outState: Bundle) {
        super.onSaveInstanceState(outState)
    }

    override fun dispatchKeyEvent(event: android.view.KeyEvent): Boolean {
        val action=org.citra.citra_emu.utils.DekuiMenuInput.action(event)
        if(action==org.citra.citra_emu.utils.DekuiMenuInput.Action.Back) {
            if(event.action==android.view.KeyEvent.ACTION_UP) {
                finish()
            }
            return true
        }
        if(action==org.citra.citra_emu.utils.DekuiMenuInput.Action.Confirm ||
            action==org.citra.citra_emu.utils.DekuiMenuInput.Action.Start) {
            if(event.action==android.view.KeyEvent.ACTION_UP)currentFocus?.takeIf { it.isEnabled }?.performClick()
            return true
        }
        val direction=org.citra.citra_emu.utils.DekuiMenuInput.direction(action)
        if(direction!=null)return super.dispatchKeyEvent(android.view.KeyEvent(
            event.downTime,event.eventTime,event.action,direction,event.repeatCount,
            event.metaState,event.deviceId,event.scanCode,event.flags,event.source))
        // Android's default Button A click handling must not bypass the saved bindings.
        if(android.view.KeyEvent.isGamepadButton(event.keyCode))return true
        return super.dispatchKeyEvent(event)
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
            result.fold({ buildLauncher();openSettings("Graphics") }, {
                status.text = it.message ?: "Game preparation failed. Please select the supported game file."
                select.isEnabled = true
                select.text = "Select ROM"
            })
        }
    }
    private fun openSettings(page: String="General") {
        startActivity(Intent(this,DekuiSettingsActivity::class.java).putExtra("page",page))
    }
    private fun openGame() {
        if(org.citra.citra_emu.utils.DekuiAdultLinkSetup.generating) {
            android.widget.Toast.makeText(this,"Wait for Adult Link generation to finish",android.widget.Toast.LENGTH_LONG).show();return
        }

        runCatching { org.citra.citra_emu.utils.DekuiHudUpgrade.apply(this) }.onFailure {
            android.widget.Toast.makeText(this, "PC HUD upgrade: ${it.message}", android.widget.Toast.LENGTH_LONG).show()
        }
        runCatching { org.citra.citra_emu.utils.DekuiPcControls.apply(this) }.onFailure {
            status.text = "PC controls upgrade: ${it.message}"
            return
        }
        org.citra.citra_emu.features.settings.model.Settings().loadSettings()
        if (org.citra.citra_emu.features.settings.model.BooleanSetting.CUSTOM_TEXTURES.boolean && !DekuiTextureStorage.readable()) {
            openSettings("Graphics")
            status.text = "Texture folder is unavailable. Reconnect storage or choose the folder again."
            return
        }
        startActivity(Intent(this,EmulationActivity::class.java).apply {
            action = Intent.ACTION_VIEW
            data = Uri.fromFile(game)
            putExtra("launched_from_shortcut",true)
        })
        finish()
    }
}
