// SPDX-License-Identifier: GPL-2.0-or-later
package org.citra.citra_emu.activities

import android.content.res.ColorStateList
import android.content.Intent
import android.graphics.Color
import android.os.Bundle
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.Gravity
import android.view.View
import android.widget.*
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import androidx.preference.PreferenceManager
import org.citra.citra_emu.BuildConfig
import org.citra.citra_emu.NativeLibrary
import org.citra.citra_emu.R
import org.citra.citra_emu.display.ReMaskedCast
import org.citra.citra_emu.display.SecondaryDisplayLayout
import org.citra.citra_emu.features.settings.model.*
import org.citra.citra_emu.features.settings.utils.SettingsFile
import org.citra.citra_emu.utils.*
import kotlin.math.roundToInt

/** The same Settings view opens from the launcher and during gameplay. */
class DekuiSettingsActivity : AppCompatActivity() {
    private lateinit var store: DekuiPresentationStore
    private lateinit var menu: ReMaskedSettingsView
    private val content get() = menu.content
    private var page="General"
    private var captureRelease=false
    private var generatingMod=false
    private val prefs by lazy { PreferenceManager.getDefaultSharedPreferences(this) }
    private val purple = ReMaskedSettingsView.ACCENT
    private val pages = listOf("General", "Controls", "Touch", "Graphics", "Audio", "HUD",
        "Difficulty", "Restoration Fixes", "Quality of life", "Models", "About", "Data", "Cheats")
    private val inGame by lazy { intent.getBooleanExtra(GAMEPLAY, false) }
    private var presentationChanged = false
    private var configChanged = false

    companion object {
        const val GAMEPLAY = "remasked.gameplay"
        const val PRESENTATION_CHANGED = "remasked.presentationChanged"
        const val CONFIG_CHANGED = "remasked.configChanged"
        const val GAME_ACTION = "remasked.gameAction"
        const val EDIT_TOUCH_LAYOUT = "editTouchLayout"
        const val OPEN_GAME_TOOLS = "openGameTools"
        const val CHOOSE_ROM = "chooseRom"
    }

    private var size = 100
    private data class Choice(val title: String, val options: List<String>, val selected: Int, val write: (Int) -> Unit)
    private var openChoice: Choice? = null
    private var choiceReturnScroll = 0
    private var restoreScroll: Int? = null
    private var editingSlider = false
    private val picker=registerForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri ->
        if(uri!=null) {
            runCatching { DekuiTextureStorage.select(uri) }.onFailure { error(it) }
            render()
        }
    }
    private val modRomPicker=registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri->
        if(uri!=null)runCatching { DekuiAdultLinkSetup.selectRom(this,uri);render() }.onFailure { error(it) }
    }
    private val modTexturePicker=registerForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri->
        if(uri!=null)runCatching { DekuiAdultLinkSetup.selectPack(this,uri);render() }.onFailure { error(it) }
    }
    private val tpRomPicker=registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri->
        if(uri!=null)runCatching { DekuiTwilightLinkSetup.selectRom(this,uri);render() }.onFailure { error(it) }
    }
    private val tpTexturePicker=registerForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri->
        if(uri!=null)runCatching { DekuiTwilightLinkSetup.selectPack(this,uri);render() }.onFailure { error(it) }
    }
    private val pakPicker=registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri->
        if(uri!=null)generateMod(pak=uri)
    }
    @Suppress("DEPRECATION")
    private fun generateMod(dark:Boolean=false,twilight:Boolean=false,pak:android.net.Uri?=null) {
        if(generatingMod)return
        generatingMod=true
        val dialog=android.app.ProgressDialog(this).apply { setTitle(if(pak!=null)"Importing PAK player model"else if(twilight)"Creating Twilight Princess Link"else if(dark)"Creating Dark Link"else"Creating Adult Link");setMessage("Preparing…");setCancelable(false);show() }
        lifecycleScope.launch {
            val result=withContext(Dispatchers.IO){runCatching {
                val progress:(String)->Unit={message->runOnUiThread { dialog.setMessage(message) }}
                if(pak!=null)DekuiPakSetup.importModel(this@DekuiSettingsActivity,pak,progress)
                else if(twilight)DekuiTwilightLinkSetup.generate(this@DekuiSettingsActivity,progress)
                else if(dark)DekuiAdultLinkSetup.generateDarkLink(this@DekuiSettingsActivity,progress)
                else DekuiAdultLinkSetup.generate(this@DekuiSettingsActivity,progress)
            }}
            generatingMod=false;dialog.dismiss()
            result.onSuccess { Toast.makeText(this@DekuiSettingsActivity,it,Toast.LENGTH_LONG).show() }.onFailure { error(it) }
            render()
        }
    }
    private fun choice(title: String, options: List<String>, selected: Int, change: (Int) -> Unit) {
        menu.add(menu.choiceRow(title, options[selected]) { openOptions(title, options, selected, change) })
    }

    private fun openOptions(title: String, options: List<String>, selected: Int = -1, change: (Int) -> Unit) {
        choiceReturnScroll = menu.scrollPosition
        openChoice = Choice(title, options, selected, change)
        render()
    }

    private fun goBack() {
        if (editingSlider) {
            editingSlider = false
            return
        }
        if (openChoice != null) {
            openChoice = null
            restoreScroll = choiceReturnScroll
            render()
        } else finish()
    }

    override fun onCreate(state: Bundle?) {
        super.onCreate(state)
        if (NativeLibrary.isRunning() != inGame) { finish(); return }
        presentationChanged = state?.getBoolean(PRESENTATION_CHANGED) ?: false
        configChanged = state?.getBoolean(CONFIG_CHANGED) ?: false
        publishResult()
        requestedOrientation = if (inGame) android.content.pm.ActivityInfo.SCREEN_ORIENTATION_LOCKED
            else android.content.pm.ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
        onBackPressedDispatcher.addCallback(this, object : androidx.activity.OnBackPressedCallback(true) {
            override fun handleOnBackPressed() {
                if (DekuiAndroidMenu.capture >= 0) {
                    DekuiAndroidMenu.cancel()
                    render()
                } else goBack()
            }
        })
        WindowCompat.setDecorFitsSystemWindows(window,false)
        WindowInsetsControllerCompat(window,window.decorView).apply {
            hide(WindowInsetsCompat.Type.systemBars())
            systemBarsBehavior=WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }
        store=DekuiPresentationStore(this)
        page=(state?.getString("page") ?: intent.getStringExtra("page"))?.takeIf { it in pages } ?: "General"
        runCatching {
            Settings().loadSettings()
            val legacySize = 80 + store.read().optInt("android_menu_text_size", 20)
            size = prefs.getInt("ReMaskedSettingsScale", legacySize).coerceIn(50, 125)
            render()
        }
            .onFailure { error(it);finish() }
    }
    override fun onSaveInstanceState(state: Bundle) {
        state.putString("page",page)
        state.putBoolean(PRESENTATION_CHANGED, presentationChanged)
        state.putBoolean(CONFIG_CHANGED, configChanged)
        super.onSaveInstanceState(state)
    }
    private fun publishResult(action: String? = null) {
        if (!inGame) return
        setResult(RESULT_OK, Intent().apply {
            putExtra(PRESENTATION_CHANGED, presentationChanged)
            putExtra(CONFIG_CHANGED, configChanged)
            putExtra(GAME_ACTION, action)
        })
    }
    private fun returnToGame(action: String) {
        publishResult(action)
        finish()
    }
    override fun onStop() { DekuiAndroidMenu.launcherCapture=false;DekuiAndroidMenu.cancel();super.onStop() }
    private fun error(t: Throwable) { Toast.makeText(this,t.message ?: "Could not save settings",Toast.LENGTH_LONG).show() }
    private fun update(vararg values: Pair<String,Any>) {
        runCatching { store.update(values.toMap()) }.onSuccess {
            presentationChanged = true
            publishResult()
        }.onFailure { error(it) }
    }
    private fun save(vararg settings: AbstractSetting) {
        if (!SettingsFile.saveDekuiSettings(settings.asList())) {
            error(IllegalStateException("Could not save settings"))
            Settings().loadSettings()
            render()
        } else {
            configChanged = true
            publishResult()
        }
    }
    private fun addButton(text: String, action: () -> Unit) = menu.add(menu.button(text, action = action))
    private fun note(text: String) = menu.add(menu.text(text, dim = true))
    private fun section(text: String) = menu.add(menu.text(text, heading = true))
    private fun info(label: String, value: String) = menu.add(menu.info(label, value))
    private fun percent(value: Float) = "${value.roundToInt()}%"
    private fun multiplier(value: Float) = "%.2f×".format(value)
    private fun pixels(value: Float) = "${value.roundToInt()} px"
    private fun hudPercent(value: Float) = percent(100 + 5 * value)
    private fun toggle(text: String, current: Boolean, change: (Boolean) -> Unit) =
        menu.add(menu.toggle(text, current, change))
    private fun bool(text: String,key: String,default: Boolean=false) { toggle(text,store.read().optBoolean(key,default)) { update(key to it) } }
    private fun nativeBool(text: String,setting: BooleanSetting) { toggle(text,setting.boolean) { setting.boolean=it;save(setting) } }
    private fun slider(text: String,current: Float,min: Float,max: Float,step: Float=1f,
                       format: (Float) -> String = { "%.2f".format(it) }, save: (Float)->Unit) {
        val caption = menu.text(format(current), dim = true).apply {
            setTextColor(purple)
        }
        val seek=SeekBar(this).apply {
            this.max=((max-min)/step).toInt();progress=((current-min)/step).toInt().coerceIn(0,this.max)
            progressTintList=ColorStateList.valueOf(purple)
            thumb = android.graphics.drawable.GradientDrawable().apply {
                setColor(Color.WHITE)
                setSize(menu.dp(28), menu.dp(18))
                cornerRadius = menu.dp(9).toFloat()
            }
            // SeekBar centers the thumb on the track's endpoints. Reserve the
            // overhang so the rounded thumb stays visible at both extremes.
            val thumbOverhang = thumbOffset
            setPadding(thumbOverhang, 0, thumbOverhang, 0)
            minimumHeight=menu.dp(34)
            contentDescription=text
            setOnSeekBarChangeListener(object: SeekBar.OnSeekBarChangeListener {
                var touched=false
                override fun onProgressChanged(view: SeekBar,value: Int,fromUser: Boolean) {
                    val chosen=min+value*step;caption.text=format(chosen)
                    if(fromUser && !touched)save(chosen)
                }
                override fun onStartTrackingTouch(view: SeekBar) { touched=true }
                override fun onStopTrackingTouch(view: SeekBar) { touched=false;save(min+view.progress*step) }
            })
        }
        menu.add(menu.sliderRow(text, caption, seek))
    }
    private fun number(text: String,key: String,default: Float,min: Float,max: Float,step: Float=1f,
                       format: (Float) -> String = { if (step % 1f == 0f) it.roundToInt().toString() else "%.2f".format(it) }) {
        slider(text,store.read().optDouble(key,default.toDouble()).toFloat(),min,max,step,format) { update(key to it) }
    }
    private fun iconSelection() {
        val icons = intArrayOf(R.mipmap.launcher_icon_0, R.mipmap.launcher_icon_1,
            R.mipmap.launcher_icon_2, R.mipmap.launcher_icon_3, R.mipmap.launcher_icon_4)
        val selected = LauncherIconSelection.selected(this)
        val row = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            setPadding(menu.dp(14), menu.dp(12), menu.dp(14), menu.dp(12))
        }
        icons.forEachIndexed { index, drawable ->
            row.addView(object : ImageButton(this) {
                override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
                    val width = View.MeasureSpec.getSize(widthMeasureSpec)
                    super.onMeasure(widthMeasureSpec, View.MeasureSpec.makeMeasureSpec(width, View.MeasureSpec.EXACTLY))
                }
            }.apply {
                setImageResource(drawable)
                scaleType = ImageView.ScaleType.FIT_CENTER
                contentDescription = if (index == 0) "Combined icon" else "Icon $index"
                isSelected = index == selected
                isFocusable = true
                setPadding(0, 0, 0, 0)
                clipToOutline = true
                fun border(focused: Boolean) {
                    background = android.graphics.drawable.GradientDrawable().apply {
                        setColor(Color.TRANSPARENT)
                        cornerRadius = menu.dp(8).toFloat()
                        if (focused || isSelected) setStroke(menu.dp(3), if (focused) Color.WHITE else purple)
                    }
                }
                border(false)
                setOnFocusChangeListener { _, focused -> border(focused) }
                setOnClickListener {
                    if (index != selected) runCatching {
                        LauncherIconSelection.select(this@DekuiSettingsActivity, index)
                        render()
                    }.onFailure { error(it) }
                }
            }, LinearLayout.LayoutParams(0, -2, 1f).apply { if (index > 0) marginStart = menu.dp(6) })
        }
        menu.add(row)
    }
    private fun render() {
        editingSlider = false
        menu = ReMaskedSettingsView(this, size)
        val choice = openChoice
        val root = menu.root
        menu.show(pages, page, choice?.title, ::goBack) { category ->
                openChoice = null
                page = category
                render()
            }
        androidx.core.view.ViewCompat.setOnApplyWindowInsetsListener(root) { _, insets ->
            val safe = insets.getInsets(WindowInsetsCompat.Type.displayCutout())
            menu.applyInsets(safe.left, safe.top, safe.right, safe.bottom)
            insets
        }
        if (choice != null) {
            var focused: View? = null
            choice.options.forEachIndexed { index, option ->
                val item = menu.option(option, selected = index == choice.selected) {
                    openChoice = null
                    restoreScroll = choiceReturnScroll
                    val previous = menu
                    if (index != choice.selected) choice.write(index)
                    if (menu === previous) render()
                }
                if (index == choice.selected) focused = item
                menu.add(item)
            }
            setContentView(root)
            if (!root.isInTouchMode) focused?.requestFocus()
            return
        }
        when(page) {
            "General" -> {
                section("Appearance")
                val saveMenuSize: (Float) -> Unit = {
                    size = it.toInt()
                    prefs.edit().putInt("ReMaskedSettingsScale", size).apply()
                    render()
                }
                slider("Menu size", size.toFloat(), 50f, 125f, format = { "%.2f×".format(it / 100f) }, save = saveMenuSize)
                addButton("Reset menu size") { saveMenuSize(100f) }
                iconSelection()
                section("Language")
                val languages=listOf(1,2,5)
                val language=NativeLibrary.getDekuiGameLanguage()
                choice("Game language",listOf("English","Français","Español"),languages.indexOf(language).coerceAtLeast(0)) { index->
                    if(!NativeLibrary.setDekuiGameLanguage(languages[index])) {
                        error(IllegalStateException("Could not save the game language. Try selecting it again."))
                    }
                    render()
                }
                note("Applies when you start the game. Settings menus remain in English.")
                bool("Show frame rate","show_performance")
            }
            "Graphics" -> graphics()
            "Models" -> {
                if (inGame) {
                    note("Change the player model or import a PAK from startup Settings.")
                    setContentView(root)
                    menu.focusCategory()
                    return
                }
                val data=store.read()
                section("Appearance")
                val model=if(data.optBoolean("pak_link_model",false))4 else if(data.optBoolean("twilight_link_model",false))3 else if(data.optBoolean("dark_link_model",false))2 else if(data.optBoolean("adult_link_model",false))1 else 0
                choice("Player model",listOf("Original Link","Adult Link","Dark Link","Twilight Princess Link","PAK Importer"),model) { index->
                    update("pak_link_model" to (index==4),"twilight_link_model" to (index==3),"adult_link_model" to (index==1),"dark_link_model" to (index==2));render()
                }
                if(model==4) {
                    note(DekuiPakSetup.description(this))
                    content.addView(menu.button("Import PAK file") { pakPicker.launch(arrayOf("*/*")) }.apply {
                        isEnabled=DekuiPakSetup.canImport(this@DekuiSettingsActivity)&&!generatingMod
                    },LinearLayout.LayoutParams(-1,-2))
                    note("Select a ModLoader64 .pak player-model package. Uses native MM3D equipment, abilities and voices. Import before gameplay; restart after changing models. Human Link rigs are supported; transformation forms and custom scripts are not imported.")
                }
                if(model==3) {
                    toggle("Master Sword",data.optBoolean("twilight_link_master_sword",true)) { enabled->
                        update("twilight_link_master_sword" to enabled);render()
                    }
                    note("On: TP Master Sword and sheath. Off: your native MM3D sword. Appearance only; sword damage stays unchanged.")
                    note("Includes TP action voices. Cap animation, facial expressions and mask fitting are unfinished.")
                    section("Twilight Princess setup")
                    note(DekuiTwilightLinkSetup.description(this))
                    addButton("Select your Twilight Princess ROM") { tpRomPicker.launch(arrayOf("*/*")) }
                    addButton("Choose your TP texture pack") { tpTexturePicker.launch(android.net.Uri.parse("content://com.android.externalstorage.documents/document/primary%3ADusklight%2Ftexture_replacements")) }
                    content.addView(menu.button("Generate Twilight Princess Link mod") { generateMod(twilight=true) }.apply {
                        isEnabled=DekuiTwilightLinkSetup.canGenerate(this@DekuiSettingsActivity)&&!generatingMod
                    },LinearLayout.LayoutParams(-1,-2))
                    note("Reads your own GameCube ISO or RVZ and searches every subfolder of the selected texture pack. Source files and other mods are preserved.")
                }
                if(model==2) {
                    if(!java.io.File(org.citra.citra_emu.utils.DekuiStorage.root(this),"load/mm3d-models/dark-link/romfs/actors/zelda2_link_child_new.gar.lzs").isFile)
                        note("Dark Link is not installed. Generate the mod below from your OoT3D ROM before starting the game.")
                    note("Original OoT3D Dark Link model, red eyes and sword, with imported adult voice clips. Keeps normal MM3D abilities and your equipped shield. Sword damage stays unchanged.")
                }
                if(model==1) {
                    val colors=listOf("Green (Kokiri)","Blue (Zora)","Red (Goron)","Purple","Black","White","Gold")
                    val current=data.optInt("adult_link_tunic_color",0).coerceIn(0,6)
                    fun available(color:Int,master:Boolean):Boolean {
                        val variant=if(master)"master"else"kokiri"
                        val suffix=if(color==1)"-blue"else if(color==2)"-red"else""
                        return java.io.File(org.citra.citra_emu.utils.DekuiStorage.root(this),"load/mm3d-models/adult-link/variants/$variant$suffix/romfs/actors/zelda2_link_child_new.gar.lzs").isFile
                    }
                    val master=data.optBoolean("adult_link_master_sword",false)
                    if(!available(current,master))note("The selected model is not installed. Generate the mod below from your OoT3D ROM before starting the game.")
                    choice("Link's tunic color",colors,current) { index->
                        if(!available(index,master)) {
                            error(IllegalStateException("Generate the mod from your OoT3D ROM to use the original tunic colors"))
                        }else update("adult_link_tunic_color" to index)
                        render()
                    }
                    toggle("Master Sword",master) { enabled->
                        if(available(current,enabled))update("adult_link_master_sword" to enabled)
                        else error(IllegalStateException("Generate the mod from your OoT3D ROM to use this sword and tunic combination"))
                        render()
                    }
                    note("On: OoT3D Master Sword and sheath. Off: Kokiri sword and sheath. Appearance only; sword damage stays unchanged.")
                }
                if(model!=4)note("Imported models include adult voice clips. Restart the game after changing models. Facial expressions and mask fitting are unfinished.")
                if(model!=3&&model!=4) {
                section("Mod setup")
                note(DekuiAdultLinkSetup.description(this))
                addButton("Select your OoT3D ROM") { modRomPicker.launch(arrayOf("*/*")) }
                addButton("Choose your OoT3D texture pack") { modTexturePicker.launch(android.net.Uri.parse("content://com.android.externalstorage.documents/document/primary%3AROMs%2Fn3DS")) }
                content.addView(menu.button("Generate Adult Link mod") { generateMod() }.apply { isEnabled=DekuiAdultLinkSetup.canGenerate(this@DekuiSettingsActivity)&&!generatingMod },LinearLayout.LayoutParams(-1,-2))
                content.addView(menu.button("Generate Dark Link mod") { generateMod(true) }.apply { isEnabled=DekuiAdultLinkSetup.canGenerate(this@DekuiSettingsActivity)&&!generatingMod },LinearLayout.LayoutParams(-1,-2))
                note("Uses your decrypted OoT3D .3ds/.cci ROM and copies only matching textures from your pack. Original files are read-only. No full ROM copy is stored.")
                if(model==1&&!java.io.File(org.citra.citra_emu.utils.DekuiStorage.root(this),"load/mm3d-models/adult-link/romfs/actors/zelda2_link_child_new.gar.lzs").isFile)
                    note("Converted Adult Link model is not installed.")
                }
            }
            "Controls" -> controls()
            "Touch" -> {
                section("Touch controls")
                toggle("Show touch controls",EmulationMenuSettings.showOverlay) { EmulationMenuSettings.showOverlay=it }
                slider("Touch-control opacity", prefs.getInt("controlOpacity", 50).toFloat(), 0f, 100f, format = ::percent) {
                    prefs.edit().putInt("controlOpacity", it.toInt()).apply()
                }
                slider("Touch-control size", prefs.getInt("controlScale", 50) + 50f, 50f, 200f, format = ::percent) {
                    prefs.edit().putInt("controlScale", it.toInt() - 50).apply()
                }
                toggle("Touch vibration",EmulationMenuSettings.hapticFeedback) { EmulationMenuSettings.hapticFeedback=it }
                if (inGame) addButton("Edit touch-control placement") { returnToGame(EDIT_TOUCH_LAYOUT) }
                else note("Touch-control placement can be edited during gameplay.")
                addButton("Reset layout") {
                    androidx.appcompat.app.AlertDialog.Builder(this)
                        .setTitle("Reset touch layout?")
                        .setMessage("Restore the default control positions in all orientations.")
                        .setNegativeButton(android.R.string.cancel, null)
                        .setPositiveButton("Reset") { _, _ ->
                            val editor = prefs.edit()
                            prefs.all.keys.filter { it.startsWith("ReMaskedTouch-v1-") }.forEach { editor.remove(it) }
                            editor.apply()
                        }.show()
                }
                choice("Movement stick position", listOf("Fixed", "Floating"),
                    if (EmulationMenuSettings.joystickRelCenter) 1 else 0) { EmulationMenuSettings.joystickRelCenter = it == 1 }
                toggle("Equipped item artwork", prefs.getBoolean("ReMaskedTouchHud", true)) {
                    prefs.edit().putBoolean("ReMaskedTouchHud", it).apply()
                }
                choice("Face layout", listOf("Nintendo diamond", "Alternate diamond", "GameCube"),
                    prefs.getInt("ReMaskedTouchFaceLayout", 0).coerceIn(0, 2)) {
                    prefs.edit().putInt("ReMaskedTouchFaceLayout", it).apply()
                }
                section("Individual controls")
                val names = listOf("Action", "Sword", "Item X", "Item Y", "Item I", "Item II", "Target",
                    "Shield", "Start / Items menu", null, "D-pad", "Movement stick", "Camera stick")
                val controlIds = listOf(700, 701, 702, 703, 773, 774, 707, 708, 704, 705, 780, 713, 718)
                names.forEachIndexed { index, name ->
                    if (name == null) return@forEachIndexed
                    val visibility = "buttonToggle$index"
                    val scale = "controlScale-${controlIds[index]}"
                    toggle("Show $name", prefs.getBoolean(visibility, index != 12)) {
                        prefs.edit().putBoolean(visibility, it).apply()
                    }
                    slider("$name size", prefs.getInt(scale, 50) + 50f, 50f, 200f, 5f, ::percent) {
                        prefs.edit().putInt(scale, it.toInt() - 50).apply()
                    }
                }
                toggle("Show Pictograph Box", prefs.getBoolean("buttonToggle13", true)) {
                    prefs.edit().putBoolean("buttonToggle13", it).apply()
                }
                slider("Pictograph Box size", prefs.getInt("pictographScale", 50) + 50f, 50f, 200f, 5f, ::percent) {
                    prefs.edit().putInt("pictographScale", it.toInt() - 50).apply()
                }
                section("Android display")
                nativeBool("Swap screens", BooleanSetting.SWAP_SCREEN)
            }
            "Audio" -> audio()
            "HUD" -> hud()
            "Difficulty" -> difficulty()
            "Restoration Fixes" -> {
                bool("Bombers don't approach automatically","disable_bomber_approach");bool("Activate owl statues with a sword","sword_activate_owls")
                bool("Classic Song of Double Time","classic_double_time")
                if (inGame) note("Classic saving can be changed from startup Settings.")
                else bool("Classic saving","classic_saving")
            }
            "Quality of life" -> {
                section("Link movement")
                number("Walk speed (x)","walk_speed",1f,.5f,3f,.25f,::multiplier)
                number("Climb speed (x)","climb_speed",1f,.5f,3f,.25f,::multiplier)
                number("Swim speed (x)","swim_speed",1f,.5f,3f,.25f,::multiplier)
                addButton("Reset movement speeds") { update("walk_speed" to 1f,"climb_speed" to 1f,"swim_speed" to 1f);render() }
                note("1x is normal speed. Walk affects grounded movement; climb affects ladders and vines. Swim includes underwater movement.")
                bool("Disable low-heart beeping","disable_low_health_beep");bool("Disable Tatl reminders","disable_tatl_reminders") }
            "Cheats" -> {
                if (inGame) addButton("Open in-game tools") { returnToGame(OPEN_GAME_TOOLS) }
                else note("Load a game to use cheats and other in-game tools.")
            }
            "About" -> {
                info("Version", BuildConfig.VERSION_NAME)
                info("Build", BuildConfig.VERSION_CODE.toString())
            }
            "Data" -> {
                val location = if (DekuiStorage.root(this) == DekuiStorage.sharedRoot()) "Internal storage/mm3dport"
                    else "App storage"
                info("Current data folder", location)
                if (DekuiStorage.game(this).isFile) info("Game file", "Ready")
                else if (!inGame) addButton("Choose ROM") {
                    setResult(RESULT_OK, Intent().putExtra(GAME_ACTION, CHOOSE_ROM))
                    finish()
                }
            }
        }
        setContentView(root)
        restoreScroll?.let(menu::restoreScroll)
        restoreScroll = null
        menu.focusCategory()
    }
    private fun graphics() {
        section("Display")
        val aspectValues = listOf(0, 2, 1, 3, 5)
        val secondaryEnabled = BooleanSetting.ENABLE_SECONDARY_DISPLAY
        val secondaryLayout = IntSetting.SECONDARY_DISPLAY_LAYOUT
        val dualScreen = secondaryEnabled.boolean && secondaryLayout.int != SecondaryDisplayLayout.NONE.int
        val aspectTitle = if (dualScreen) "Top screen aspect ratio" else "Aspect ratio"
        choice(aspectTitle, listOf("Original 3DS (5:3)", "4:3", "16:9", "21:9", "Stretch to fill"),
            aspectValues.indexOf(IntSetting.ASPECT_RATIO.int).coerceAtLeast(0)) {
            IntSetting.ASPECT_RATIO.int = aspectValues[it]
            save(IntSetting.ASPECT_RATIO)
        }
        choice("Screen mode", listOf("Single screen", "Dual screen"), if (dualScreen) 1 else 0) { mode ->
            secondaryEnabled.boolean = mode == 1
            if (mode == 1) secondaryLayout.int = SecondaryDisplayLayout.BOTTOM_SCREEN.int
            save(secondaryEnabled, secondaryLayout)
        }
        note("Dual screen shows the 3DS touchscreen on a second display when one is available.")
        if (dualScreen) {
            val stretch = BooleanSetting.SCREEN_BOTTOM_STRETCH
            choice("Bottom screen aspect ratio", listOf("Original 3DS (4:3)", "Stretch to fill"),
                if (stretch.boolean) 1 else 0) {
                stretch.boolean = it == 1
                save(stretch)
            }
        }
        section("Rendering")
        val data = store.read()
        val interpolation = data.optBoolean("interpolation_60fps", true)
        val frameRate = if (!interpolation) 0 else if (data.optBoolean("interpolation_cpu_vertices", true)) 1 else 2
        choice("Frame rate", listOf("Original 30 FPS", "60 FPS CPU interpolation", "60 FPS GPU interpolation"), frameRate) { index ->
            update("interpolation_60fps" to (index!=0),"interpolation_cpu_vertices" to (index!=2))
            if(index!=0) { BooleanSetting.VSYNC.boolean=true;BooleanSetting.HW_SHADER.boolean=index==2;save(BooleanSetting.VSYNC);save(BooleanSetting.HW_SHADER) }
        }
        val resolutions = (1..9).map { "${it}× · ${400 * it} × ${240 * it}" }
        choice("Internal resolution", resolutions, IntSetting.RESOLUTION_FACTOR.int.coerceIn(1, 9) - 1) {
            IntSetting.RESOLUTION_FACTOR.int = it + 1
            save(IntSetting.RESOLUTION_FACTOR)
        }
        val filters = resources.getStringArray(R.array.textureFilterNames).toList()
        choice("Texture filter", filters, IntSetting.TEXTURE_FILTER.int.coerceIn(filters.indices)) {
            IntSetting.TEXTURE_FILTER.int = it
            save(IntSetting.TEXTURE_FILTER)
        }
        nativeBool("Remember compiled shaders", BooleanSetting.DISK_SHADER_CACHE)
        bool("Fix bloom", "fix_bloom", true)
        nativeBool("VSync", BooleanSetting.VSYNC)
        if (!interpolation) nativeBool("Hardware shaders", BooleanSetting.HW_SHADER)
        nativeBool("Async shader compilation", BooleanSetting.ASYNC_SHADERS)
        section("Custom textures")
        nativeBool("Use custom textures",BooleanSetting.CUSTOM_TEXTURES)
        note(DekuiTextureStorage.description())
        addButton("Choose texture folder") { picker.launch(android.net.Uri.parse("content://com.android.externalstorage.documents/document/primary%3AROMs%2Fn3DS")) }
        addButton("Use app texture folder") { DekuiTextureStorage.clear();render() }
        note("Folder changes apply on the next game launch.")
        section("Camera")
        number("Camera Distance (x)","camera_distance",1f,.5f,3f,.05f,::multiplier)
        addButton("Reset camera distance") { update("camera_distance" to 1f);render() }
        number("Field of view","game_camera_fov",65f,45f,70f,.5f)
        number("Enemy / NPC draw distance","actor_draw_distance",1f,1f,3f,.05f,::multiplier)
        number("Vegetation / prop draw distance","prop_draw_distance",1f,1f,3f,.05f,::multiplier)
        section("Casting")
        choice("Cast layout", listOf("Off", "Cast single screen gameplay", "Cast top screen"),
            ReMaskedCast.mode(this)) { ReMaskedCast.setMode(this, it) }
        addButton("Choose cast display…") {
            try {
                startActivity(Intent(android.provider.Settings.ACTION_CAST_SETTINGS))
            } catch (_: android.content.ActivityNotFoundException) {
                note("Open Cast from your Android device’s Quick Settings.")
            }
        }
        note("Single screen uses this device as a touch controller. Top screen keeps the 3DS touchscreen and neutral controls on this device.")
    }
    private fun audio() {
        section("Audio")
        val volume = ScaledFloatSetting.AUDIO_VOLUME
        slider("Volume", volume.float, 0f, 100f, format = ::percent) {
            val wasMuted = volume.float == 0f
            if (it == 0f && !wasMuted) prefs.edit().putFloat("ReMaskedUnmutedVolume", volume.float).apply()
            volume.float = it
            save(volume)
            if (wasMuted != (it == 0f)) {
                restoreScroll = menu.scrollPosition
                render()
            }
        }
        toggle("Mute", volume.float == 0f) { muted ->
            if (muted) prefs.edit().putFloat("ReMaskedUnmutedVolume", volume.float).apply()
            volume.float = if (muted) 0f else prefs.getFloat("ReMaskedUnmutedVolume", 100f).coerceIn(1f, 100f)
            save(volume)
            restoreScroll = menu.scrollPosition
            render()
        }
        bool("Mute music","mute_music")
        section("Android audio")
        bool("Accurate HLE audio","accurate_hle_audio",true)
    }
    private fun difficulty() {
        val modifiers = listOf(
            "Weaker enhanced defense" to "weaker_enhanced_defense",
            "Disable heart drops" to "disable_heart_drops",
            "Disable rupee drops" to "disable_rupee_drops",
            "Disable fairy drops" to "disable_fairy_drops",
            "Disable magic drops" to "disable_magic_drops",
            "Disable ammo pickups" to "disable_ammo_pickups",
            "Lose rupees on death" to "lose_rupees_on_death",
            "Dying moves time forward 6 hours" to "advance_six_hours_on_death",
            "Fast time" to "fast_time",
            "World ends on death" to "world_ends_on_death",
            "Increase purchase prices and fees" to "double_prices",
        )
        number("Damage taken", "damage_multiplier", 1f, 1f, 16f, format = ::multiplier)
        number("Red potion healing", "red_potion_hearts", 20f, 1f, 20f)
        number("Blue potion healing", "blue_potion_hearts", 20f, 1f, 20f)
        number("Bottled fairy healing", "bottled_fairy_hearts", 10f, 1f, 20f)
        modifiers.forEach { bool(it.first, it.second) }
        number("Price multiplier", "price_multiplier", 2f, 2f, 3f, format = ::multiplier)
        addButton("Reset difficulty") {
            update(*modifiers.map { it.second to false }.toTypedArray(),
                "damage_multiplier" to 1, "red_potion_hearts" to 20, "blue_potion_hearts" to 20,
                "bottled_fairy_hearts" to 10, "price_multiplier" to 2)
            render()
        }
    }
    private fun controls() {
        section("Game prompts")
        bool("Hide item menu button prompts","player_hud_hide_menu_prompts");bool("Single-screen game instructions","single_screen_text",true)
        bool("Controller neutral item prompts","controller_neutral_item_prompts",true)
        section("Controller bindings")
        val labels=listOf("A","B","X","Y","Select","Start","Home","ZL / L2: targeting","ZR / R2: shield","L / L1: item I","R / R1: item II","D-pad up","D-pad down","D-pad left","D-pad right","Left stick vertical","Left stick horizontal","Right stick vertical","Right stick horizontal","D-pad vertical axis","D-pad horizontal axis","Pictograph Box")
        fun binding(index: Int) {
            val label = labels[index]
            val capturing = DekuiAndroidMenu.capture == index
            val value = if (capturing) "Press a button or move an axis…" else DekuiAndroidMenu.bindingLabel(index).ifEmpty { "Unbound" }
            menu.add(menu.choiceRow(label, value) {
                if (capturing) {
                    DekuiAndroidMenu.cancel()
                    restoreScroll = menu.scrollPosition
                    render()
                } else {
                    openOptions(label, listOf("Assign control", "Clear binding")) { selected ->
                        if (selected == 0) {
                            DekuiAndroidMenu.launcherCapture = true
                            DekuiAndroidMenu.begin(index)
                        } else DekuiAndroidMenu.clear(index)
                    }
                }
            })
        }
        (0..14).filter { it != 6 }.forEach(::binding)
        binding(labels.lastIndex)
        addButton("Restore ReMasked controller layout") {
            androidx.appcompat.app.AlertDialog.Builder(this)
                .setTitle("Restore controller layout?")
                .setMessage("Replace your current game bindings with the ReMasked defaults.")
                .setNegativeButton(android.R.string.cancel, null)
                .setPositiveButton("Restore") { _, _ ->
                    runCatching {
                        org.citra.citra_emu.features.settings.model.view.InputBindingSetting.applyDekuiPcSnapshotBindings()
                        prefs.edit().remove(DekuiAndroidMenu.PICTOGRAPH_KEY).apply()
                    }.onFailure(::error)
                    restoreScroll = menu.scrollPosition
                    render()
                }.show()
        }
        section("Sticks and triggers")
        (15..20).forEach(::binding)
        section("Android controls")
        binding(6)
        if(DekuiAndroidMenu.capture>=0)addButton("Cancel binding") { DekuiAndroidMenu.cancel();render() }
    }
    private fun hud() {
        section("Overall HUD")
        number("Overall HUD scale","player_hud_scale",0f,-10f,10f,format=::hudPercent)
        number("Overall HUD opacity","player_hud_opacity",1f,0f,1f,.01f) { percent(it * 100) }
        number("Heart spacing","player_hud_heart_spacing",0f,-40f,40f)
        number("Magic meter height","player_hud_magic_height",0f,-10f,10f,format=::hudPercent)
        val elements = listOf("D-pad" to "dpad", "Magic meter" to "magic", "Hearts" to "hearts",
            "Buttons" to "buttons", "Clock" to "clock", "Rupees" to "rupees",
            "Minigame counter" to "minigame", "Epona carrots" to "carrots", "Timer" to "timer")
        fun element(label: String, key: String) {
            section(label)
            number("Size","player_hud_${key}_scale",0f,-10f,10f,format=::hudPercent)
            number("Horizontal offset","player_hud_${key}_x",0f,-400f,400f,format=::pixels)
            number("Vertical offset","player_hud_${key}_y",0f,-240f,240f,format=::pixels)
        }
        elements.forEach { (label, key) -> element(label, key) }
        addButton("Reset gameplay HUD") {
            val positions = elements.flatMap { (_, key) -> listOf("scale", "x", "y").map { "player_hud_${key}_$it" to 0 } }
            update(*positions.toTypedArray(), "player_hud_scale" to 0, "player_hud_opacity" to 1f,
                "player_hud_heart_spacing" to 0, "player_hud_magic_height" to 0, "player_hud_hide_menu_prompts" to false)
            restoreScroll = menu.scrollPosition
            render()
        }
        section("Title screen")
        val poses = listOf("Logo" to "logo", "Copyright" to "copyright", "Press Start" to "press_start", "Loading icon" to "loading_icon")
        fun titleDefaults(key: String) = listOf("startup_${key}_x" to 0f, "startup_${key}_y" to 0f, "startup_${key}_scale" to 1f)
        for ((label, key) in poses) {
            number("$label horizontal offset","startup_${key}_x",0f,-400f,400f,.1f) { "%.2f px".format(it) }
            number("$label vertical offset","startup_${key}_y",0f,-240f,240f,.1f) { "%.2f px".format(it) }
            number("$label size","startup_${key}_scale",1f,.1f,3f,.01f) { percent(it * 100) }
            addButton("Reset $label") {
                update(*titleDefaults(key).toTypedArray())
                restoreScroll = menu.scrollPosition
                render()
            }
        }
        addButton("Reset title screen") {
            update(*poses.flatMap { (_, key) -> titleDefaults(key) }.toTypedArray())
            restoreScroll = menu.scrollPosition
            render()
        }
        section("Android HUD")
        number("Top padding","player_hud_top_padding",0f,0f,40f,format=::pixels)
        number("Bottom padding","player_hud_bottom_padding",0f,0f,40f,format=::pixels)
        number("Location banner vertical offset","player_hud_banner_y",0f,-240f,240f,format=::pixels)
        number("Location banner darkness","player_hud_banner_darkness",0f,0f,1f,.05f) { percent(it * 100) }
        element("Minimap", "minimap")
        number("Minimap opacity","minimap_opacity",.85f,0f,1f,.05f) { percent(it * 100) }
        section("Individual button offsets")
        for (key in listOf("x","y","i","ii","a","b")) {
            val label = key.uppercase()
            number("$label horizontal offset","player_hud_button_${key}_x",0f,-400f,400f,format=::pixels)
            number("$label vertical offset","player_hud_button_${key}_y",0f,-240f,240f,format=::pixels)
        }
    }
    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if(DekuiAndroidMenu.capture>=0) {
            DekuiAndroidMenu.onKey(event);if(DekuiAndroidMenu.capture<0) { captureRelease=true;render() };return true
        }
        if(captureRelease && event.action==KeyEvent.ACTION_UP) { captureRelease=false;return true }
        when(DekuiMenuInput.action(event)) {
            DekuiMenuInput.Action.Back -> {
                if(event.action==KeyEvent.ACTION_UP) onBackPressedDispatcher.onBackPressed()
                return true
            }
            DekuiMenuInput.Action.Confirm,DekuiMenuInput.Action.Start -> {
                if (event.action == KeyEvent.ACTION_UP) {
                    val focused = currentFocus
                    if (focused is SeekBar) editingSlider = !editingSlider
                    else focused?.performClick()
                }
                return true
            }
            DekuiMenuInput.Action.Left -> {
                if (!editingSlider && menu.isContentFocused()) {
                    if (event.action == KeyEvent.ACTION_DOWN) menu.focusCategory()
                    return true
                }
            }
            DekuiMenuInput.Action.Right -> {
                if (menu.isCategoryFocused()) {
                    if (event.action == KeyEvent.ACTION_DOWN) menu.focusFirstControl()
                    return true
                }
                if (!editingSlider && currentFocus is SeekBar) return true
            }
            else -> Unit
        }
        val direction=DekuiMenuInput.direction(DekuiMenuInput.action(event))
        if(direction!=null)return super.dispatchKeyEvent(KeyEvent(event.downTime,event.eventTime,event.action,direction,event.repeatCount,event.metaState,event.deviceId,event.scanCode,event.flags,event.source))
        if(KeyEvent.isGamepadButton(event.keyCode))return true
        return super.dispatchKeyEvent(event)
    }
    override fun dispatchGenericMotionEvent(event: MotionEvent): Boolean {
        if(DekuiAndroidMenu.capture>=0) { DekuiAndroidMenu.onMotion(event);if(DekuiAndroidMenu.capture<0)render();return true }
        return super.dispatchGenericMotionEvent(event)
    }
}
