// SPDX-License-Identifier: GPL-2.0-or-later
package org.citra.citra_emu.activities

import android.content.res.ColorStateList
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
import androidx.appcompat.widget.SwitchCompat
import androidx.core.content.res.ResourcesCompat
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import androidx.preference.PreferenceManager
import org.citra.citra_emu.NativeLibrary
import org.citra.citra_emu.R
import org.citra.citra_emu.features.settings.model.*
import org.citra.citra_emu.features.settings.utils.SettingsFile
import org.citra.citra_emu.utils.*

/** Full settings before launch; reads/writes the exact files used by the in-game menu. */
class DekuiSettingsActivity : AppCompatActivity() {
    private lateinit var store: DekuiPresentationStore
    private lateinit var content: LinearLayout
    private var page="General"
    private var captureRelease=false
    private var generatingMod=false
    private val prefs by lazy { PreferenceManager.getDefaultSharedPreferences(this) }
    private val purple=Color.rgb(185,125,242)
    private val pages=listOf("General","Controls","Touch","Graphics","Audio","HUD","Difficulty","Restoration","Quality of life","Mods","Cheats","About")
    private val font by lazy { ResourcesCompat.getFont(this,R.font.dekui_sans_regular) }
    private var size=20
    private fun px(v: Float)=(v*resources.displayMetrics.heightPixels/1080f).toInt()
    private fun textSize()=13f*(1.8f+.06f*size)*resources.displayMetrics.heightPixels/1080f/resources.displayMetrics.scaledDensity
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
    @Suppress("DEPRECATION")
    private fun generateMod(dark:Boolean=false) {
        if(generatingMod)return
        generatingMod=true
        val dialog=android.app.ProgressDialog(this).apply { setTitle(if(dark)"Creating Dark Link"else"Creating Adult Link");setMessage("Preparing…");setCancelable(false);show() }
        lifecycleScope.launch {
            val result=withContext(Dispatchers.IO){runCatching {
                val progress:(String)->Unit={message->runOnUiThread { dialog.setMessage(message) }}
                if(dark)DekuiAdultLinkSetup.generateDarkLink(this@DekuiSettingsActivity,progress)
                else DekuiAdultLinkSetup.generate(this@DekuiSettingsActivity,progress)
            }}
            generatingMod=false;dialog.dismiss()
            result.onSuccess { Toast.makeText(this@DekuiSettingsActivity,it,Toast.LENGTH_LONG).show() }.onFailure { error(it) }
            render()
        }
    }
    private fun choice(title:String,options:List<String>,selected:Int,change:(Int)->Unit) {
        content.addView(label(title))
        val adapter=object:ArrayAdapter<String>(this,android.R.layout.simple_spinner_item,options) {
            override fun getView(position:Int,convertView:View?,parent:android.view.ViewGroup):View = label(getItem(position)!!+"  ▾").apply { setBackgroundColor(0xFF291C38.toInt()) }
            override fun getDropDownView(position:Int,convertView:View?,parent:android.view.ViewGroup):View = label(getItem(position)!!).apply { setBackgroundColor(if(position==selected)0xFF3D1468.toInt()else 0xFF291C38.toInt()) }
        }
        content.addView(Spinner(this,Spinner.MODE_DROPDOWN).apply {
            this.adapter=adapter;setSelection(selected)
            onItemSelectedListener=object:AdapterView.OnItemSelectedListener {
                override fun onNothingSelected(parent:AdapterView<*>?) {}
                override fun onItemSelected(parent:AdapterView<*>?,view:View?,position:Int,id:Long) { if(position!=selected)change(position) }
            }
        },LinearLayout.LayoutParams(-1,px(80f)))
    }
    override fun onCreate(state: Bundle?) {
        super.onCreate(state)
        if(NativeLibrary.isRunning()) { finish();return }
        WindowCompat.setDecorFitsSystemWindows(window,false)
        WindowInsetsControllerCompat(window,window.decorView).apply {
            hide(WindowInsetsCompat.Type.systemBars())
            systemBarsBehavior=WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }
        store=DekuiPresentationStore(this)
        page=(state?.getString("page") ?: intent.getStringExtra("page"))?.takeIf { it in pages } ?: "General"
        runCatching { Settings().loadSettings();size=store.read().optInt("android_menu_text_size",20);render() }
            .onFailure { error(it);finish() }
    }
    override fun onSaveInstanceState(state: Bundle) { state.putString("page",page);super.onSaveInstanceState(state) }
    override fun onStop() { DekuiAndroidMenu.launcherCapture=false;DekuiAndroidMenu.cancel();super.onStop() }
    private fun error(t: Throwable) { Toast.makeText(this,t.message ?: "Could not save settings",Toast.LENGTH_LONG).show() }
    private fun update(vararg values: Pair<String,Any>) { runCatching { store.update(values.toMap()) }.onFailure { error(it) } }
    private fun save(setting: AbstractSetting) {
        if(!SettingsFile.saveDekuiSettings(listOf(setting))) { error(IllegalStateException("Could not save settings"));Settings().loadSettings();render() }
    }
    private fun label(value: String,dim: Boolean=false)=TextView(this).apply {
        text=value;typeface=font;textSize=textSize();setTextColor(if(dim)0xFFADA4BD.toInt() else Color.rgb(242,242,242))
        setPadding(px(12f),px(10f),px(12f),px(10f))
    }
    private fun button(label: String,action: ()->Unit)=Button(this).apply {
        text=label;typeface=font;textSize=textSize();isAllCaps=false;setTextColor(Color.WHITE)
        backgroundTintList=ColorStateList(arrayOf(intArrayOf(android.R.attr.state_focused),intArrayOf()),intArrayOf(0xFF5A2386.toInt(),0xFF291C38.toInt()))
        minHeight=px(70f);minimumHeight=px(70f);includeFontPadding=false;setPadding(px(14f),px(8f),px(14f),px(8f));setOnClickListener { action() }
    }
    private fun addButton(text: String,action: ()->Unit) { content.addView(button(text,action),LinearLayout.LayoutParams(-1,-2)) }
    private fun note(text: String) { content.addView(label(text,true)) }
    private fun section(text: String) { content.addView(label(text).apply { setTextColor(purple) }) }
    private fun toggle(text: String,current: Boolean,change: (Boolean)->Unit) {
        val row=SwitchCompat(this).apply {
            this.text=text;typeface=font;textSize=textSize();setTextColor(Color.WHITE)
            isChecked=current;minHeight=px(76f);setPadding(px(14f),px(8f),px(14f),px(8f))
            thumbTintList=ColorStateList.valueOf(purple)
            setOnCheckedChangeListener { _,value->change(value) }
        }
        content.addView(row,LinearLayout.LayoutParams(-1,-2))
    }
    private fun bool(text: String,key: String,default: Boolean=false) { toggle(text,store.read().optBoolean(key,default)) { update(key to it) } }
    private fun nativeBool(text: String,setting: BooleanSetting) { toggle(text,setting.boolean) { setting.boolean=it;save(setting) } }
    private fun slider(text: String,current: Float,min: Float,max: Float,step: Float=1f,save: (Float)->Unit) {
        val caption=label("$text: ${"%.2f".format(current)}")
        content.addView(caption)
        val seek=SeekBar(this).apply {
            this.max=((max-min)/step).toInt();progress=((current-min)/step).toInt().coerceIn(0,this.max)
            progressTintList=ColorStateList.valueOf(purple);thumbTintList=ColorStateList.valueOf(purple)
            minimumHeight=px(65f)
            setOnSeekBarChangeListener(object: SeekBar.OnSeekBarChangeListener {
                var touched=false
                override fun onProgressChanged(view: SeekBar,value: Int,fromUser: Boolean) {
                    val chosen=min+value*step;caption.text="$text: ${"%.2f".format(chosen)}"
                    if(fromUser && !touched)save(chosen)
                }
                override fun onStartTrackingTouch(view: SeekBar) { touched=true }
                override fun onStopTrackingTouch(view: SeekBar) { touched=false;save(min+view.progress*step) }
            })
        }
        content.addView(seek,LinearLayout.LayoutParams(-1,px(70f)))
    }
    private fun number(text: String,key: String,default: Float,min: Float,max: Float,step: Float=1f) {
        slider(text,store.read().optDouble(key,default.toDouble()).toFloat(),min,max,step) { update(key to it) }
    }
    private fun render() {
        val root=LinearLayout(this).apply { orientation=LinearLayout.VERTICAL;setBackgroundColor(0xFF08070D.toInt());setPadding(px(30f),px(22f),px(30f),px(22f)) }
        val header=LinearLayout(this).apply { gravity=Gravity.CENTER_VERTICAL }
        header.addView(label("DekUI / Settings").apply { setTextColor(purple) },LinearLayout.LayoutParams(0,-2,1f))
        header.addView(button("Back") { finish() },LinearLayout.LayoutParams(px(240f),-2));root.addView(header)
        val body=LinearLayout(this)
        val rail=LinearLayout(this).apply { orientation=LinearLayout.VERTICAL }
        pages.forEach { name->rail.addView(button(name) { page=name;render() }.apply {
            if(page==name)backgroundTintList=ColorStateList.valueOf(0xFF3D1468.toInt())
        },LinearLayout.LayoutParams(-1,-2)) }
        body.addView(ScrollView(this).apply { addView(rail) },LinearLayout.LayoutParams(0,-1,.24f))
        content=LinearLayout(this).apply { orientation=LinearLayout.VERTICAL;setPadding(px(30f),px(10f),px(12f),px(20f));setBackgroundColor(0xFF100D17.toInt()) }
        section(page)
        body.addView(ScrollView(this).apply { isFillViewport=true;addView(content) },LinearLayout.LayoutParams(0,-1,.76f))
        root.addView(body,LinearLayout.LayoutParams(-1,0,1f))
        when(page) {
            "General" -> {
                note("Game data: Internal storage/mm3dport\nHUD settings, controller bindings, saves and mods are stored here.")
                bool("Show frame rate","show_performance")
                slider("Menu size",size.toFloat(),0f,40f) { update("android_menu_text_size" to it.toInt());size=it.toInt();render() }
                addButton("Reset menu size") { update("android_menu_text_size" to 20);size=20;render() }
                note("Settings are saved as you change them and are shared with the in-game menu.")
            }
            "Graphics" -> graphics()
            "Mods" -> {
                val data=store.read()
                section("Appearance")
                val model=if(data.optBoolean("dark_link_model",false))2 else if(data.optBoolean("adult_link_model",false))1 else 0
                choice("Player model",listOf("Original Link","Adult Link","Dark Link"),model) { index->
                    update("adult_link_model" to (index==1),"dark_link_model" to (index==2));render()
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
                note("Imported models include adult voice clips. Restart the game after changing models. Facial expressions and mask fitting are unfinished.")
                section("Mod setup")
                note(DekuiAdultLinkSetup.description(this))
                addButton("Select your OoT3D ROM") { modRomPicker.launch(arrayOf("*/*")) }
                addButton("Choose your OoT3D texture pack") { modTexturePicker.launch(android.net.Uri.parse("content://com.android.externalstorage.documents/document/primary%3AROMs%2Fn3DS")) }
                content.addView(button("Generate Adult Link mod") { generateMod() }.apply { isEnabled=DekuiAdultLinkSetup.canGenerate(this@DekuiSettingsActivity)&&!generatingMod },LinearLayout.LayoutParams(-1,-2))
                content.addView(button("Generate Dark Link mod") { generateMod(true) }.apply { isEnabled=DekuiAdultLinkSetup.canGenerate(this@DekuiSettingsActivity)&&!generatingMod },LinearLayout.LayoutParams(-1,-2))
                note("Uses your decrypted OoT3D .3ds/.cci ROM and copies only matching textures from your pack. Original files are read-only. No full ROM copy is stored.")
                if(model==1&&!java.io.File(org.citra.citra_emu.utils.DekuiStorage.root(this),"load/mm3d-models/adult-link/romfs/actors/zelda2_link_child_new.gar.lzs").isFile)
                    note("Converted Adult Link model is not installed.")
            }
            "Controls" -> controls()
            "Touch" -> {
                toggle("Show touch controls",EmulationMenuSettings.showOverlay) { EmulationMenuSettings.showOverlay=it }
                slider("Touch-control opacity",prefs.getInt("controlOpacity",50).toFloat(),0f,100f) { prefs.edit().putInt("controlOpacity",it.toInt()).apply() }
                slider("Touch-control size",prefs.getInt("controlScale",50).toFloat(),0f,150f) { prefs.edit().putInt("controlScale",it.toInt()).apply() }
                toggle("Touch vibration",EmulationMenuSettings.hapticFeedback) { EmulationMenuSettings.hapticFeedback=it }
                toggle("Swap screens",BooleanSetting.SWAP_SCREEN.boolean) { BooleanSetting.SWAP_SCREEN.boolean=it;save(BooleanSetting.SWAP_SCREEN) }
                note("Touch-button placement and visible-button selection can be edited in the Touch page during play.")
            }
            "Audio" -> {
                bool("Accurate HLE audio","accurate_hle_audio",true);bool("Mute music","mute_music")
                slider("Volume",ScaledFloatSetting.AUDIO_VOLUME.float,0f,100f) { ScaledFloatSetting.AUDIO_VOLUME.float=it;save(ScaledFloatSetting.AUDIO_VOLUME) }
            }
            "HUD" -> hud()
            "Difficulty" -> {
                number("Damage taken","damage_multiplier",1f,1f,16f)
                number("Red potion healing","red_potion_hearts",20f,1f,20f);number("Blue potion healing","blue_potion_hearts",20f,1f,20f)
                number("Bottled fairy healing","bottled_fairy_hearts",10f,1f,20f)
                listOf("Weaker enhanced defense" to "weaker_enhanced_defense","Disable heart drops" to "disable_heart_drops","Disable rupee drops" to "disable_rupee_drops","Disable fairy drops" to "disable_fairy_drops","Disable magic drops" to "disable_magic_drops","Disable ammo pickups" to "disable_ammo_pickups","Lose rupees on death" to "lose_rupees_on_death","World ends on death" to "world_ends_on_death","Increase purchase prices and fees" to "double_prices").forEach { bool(it.first,it.second) }
                number("Price multiplier","price_multiplier",2f,2f,3f)
            }
            "Restoration" -> {
                bool("Bombers don't approach automatically","disable_bomber_approach");bool("Activate owl statues with a sword","sword_activate_owls")
                bool("Classic Song of Double Time","classic_double_time");bool("Classic saving","classic_saving")
                note("These experimental options change gameplay and saving rules.")
            }
            "Quality of life" -> {
                section("Link movement")
                number("Walk speed (x)","walk_speed",1f,.5f,3f,.25f)
                number("Climb speed (x)","climb_speed",1f,.5f,3f,.25f)
                number("Swim speed (x)","swim_speed",1f,.5f,3f,.25f)
                addButton("Reset movement speeds") { update("walk_speed" to 1f,"climb_speed" to 1f,"swim_speed" to 1f);render() }
                note("1x is normal speed. Walk affects grounded movement; climb affects ladders and vines. Swim includes underwater movement.")
                bool("Disable low-heart beeping","disable_low_health_beep");bool("Disable Tatl reminders","disable_tatl_reminders") }
            "Cheats" -> note("Load your saved game to use cheats from the in-game menu. Availability is checked against the running game.")
            "About" -> note("DekUI uses Azahar and Project Restoration. \n\n${org.citra.citra_emu.BuildConfig.VERSION_NAME}")
        }
        setContentView(root)
    }
    private fun graphics() {
        val data=store.read();val selected=if(!data.optBoolean("interpolation_60fps",true))0 else if(data.optBoolean("interpolation_cpu_vertices",true))1 else 2
        listOf("Original 30 FPS","60 FPS CPU interpolation","60 FPS GPU interpolation").forEachIndexed { index,text->
            addButton((if(selected==index)"✓  " else "")+text) {
                update("interpolation_60fps" to (index!=0),"interpolation_cpu_vertices" to (index!=2))
                if(index!=0) { BooleanSetting.VSYNC.boolean=true;BooleanSetting.HW_SHADER.boolean=index==2;SettingsFile.saveDekuiSettings(listOf(BooleanSetting.VSYNC,BooleanSetting.HW_SHADER)) }
                render()
            }
        }
        number("Field of View","game_camera_fov",65f,45f,70f,.5f)
        number("Camera Distance (x)","camera_distance",1f,.5f,3f,.05f)
        addButton("Reset camera distance") { update("camera_distance" to 1f);render() }
        note("1x is normal camera distance. Higher values move farther from Link. Wall collision can bring the camera closer.")
        number("Enemy / NPC draw distance","actor_draw_distance",1f,1f,3f,.05f)
        number("Vegetation / prop draw distance","prop_draw_distance",1f,1f,3f,.05f)
        slider("Internal resolution",IntSetting.RESOLUTION_FACTOR.int.toFloat(),1f,9f) { IntSetting.RESOLUTION_FACTOR.int=it.toInt();save(IntSetting.RESOLUTION_FACTOR) }
        bool("Fix bloom","fix_bloom",true);nativeBool("VSync",BooleanSetting.VSYNC)
        nativeBool("Remember compiled shaders",BooleanSetting.DISK_SHADER_CACHE)
        if(!data.optBoolean("interpolation_60fps",true))nativeBool("Hardware shaders",BooleanSetting.HW_SHADER)
        nativeBool("Async shader compilation",BooleanSetting.ASYNC_SHADERS)
        section("Custom textures")
        nativeBool("Use custom textures",BooleanSetting.CUSTOM_TEXTURES)
        note(DekuiTextureStorage.description())
        addButton("Choose texture folder") { picker.launch(android.net.Uri.parse("content://com.android.externalstorage.documents/document/primary%3AROMs%2Fn3DS")) }
        addButton("Use app texture folder") { DekuiTextureStorage.clear();render() }
        note("Use your existing Azahar textures directly. Folder changes apply on the next game launch.")
    }
    private fun controls() {
        bool("Hide item menu button prompts","player_hud_hide_menu_prompts");bool("Single-screen game instructions","single_screen_text",true)
        bool("Controller neutral item prompts","controller_neutral_item_prompts",true)
        val labels=listOf("A","B","X","Y","Select","Start","Home","L","R","ZL","ZR","D-pad up","D-pad down","D-pad left","D-pad right","Left stick vertical","Left stick horizontal","Right stick vertical","Right stick horizontal","D-pad vertical axis","D-pad horizontal axis")
        DekuiAndroidMenu.keys.forEachIndexed { index,key->
            val value=if(DekuiAndroidMenu.capture==index)"Press a button or move an axis…" else (prefs.getString(key,"") ?: "").substringAfter(": ").ifEmpty { "Unbound" }
            addButton("${labels[index]}: $value") { DekuiAndroidMenu.launcherCapture=true;DekuiAndroidMenu.begin(index);render() }
            addButton("Clear ${labels[index]}") { DekuiAndroidMenu.clear(index);render() }
        }
        if(DekuiAndroidMenu.capture>=0)addButton("Cancel binding") { DekuiAndroidMenu.cancel();render() }
    }
    private fun hud() {
        number("Overall HUD scale","player_hud_scale",0f,-10f,10f)
        number("Overall HUD opacity","player_hud_opacity",1f,0f,1f,.05f)
        number("Top padding","player_hud_top_padding",0f,0f,40f);number("Bottom padding","player_hud_bottom_padding",0f,0f,40f)
        number("Location banner Y","player_hud_banner_y",0f,-240f,240f);number("Location banner darkness","player_hud_banner_darkness",0f,0f,1f,.05f)
        val keys=listOf("dpad","magic","hearts","buttons","minimap","clock","rupees","minigame","carrots","timer")
        val labels=listOf("D-pad","Magic meter","Hearts","Buttons","Minimap","Clock","Rupees","Minigame counter","Epona carrots","Timer")
        keys.forEachIndexed { i,key->addButton(labels[i]) {
            content.removeViews(1,content.childCount-1)
            section(labels[i]);addButton("Back to HUD") { render() }
            number("Scale","player_hud_${key}_scale",0f,-10f,10f)
            number("X position","player_hud_${key}_x",0f,-400f,400f);number("Y position","player_hud_${key}_y",0f,-240f,240f)
            if(key=="hearts")number("Heart spacing","player_hud_heart_spacing",0f,-40f,40f)
            if(key=="magic")number("Height","player_hud_magic_height",0f,-10f,10f)
            if(key=="minimap")number("Opacity","minimap_opacity",.85f,0f,1f,.05f)
            if(key=="buttons")listOf("x","y","i","ii","a","b").forEach { b->
                section(b.uppercase());number("X position","player_hud_button_${b}_x",0f,-400f,400f);number("Y position","player_hud_button_${b}_y",0f,-240f,240f)
            }
        } }
    }
    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if(DekuiAndroidMenu.capture>=0) {
            DekuiAndroidMenu.onKey(event);if(DekuiAndroidMenu.capture<0) { captureRelease=true;render() };return true
        }
        if(captureRelease && event.action==KeyEvent.ACTION_UP) { captureRelease=false;return true }
        when(DekuiMenuInput.action(event)) {
            DekuiMenuInput.Action.Back -> { if(event.action==KeyEvent.ACTION_UP)finish();return true }
            DekuiMenuInput.Action.Confirm,DekuiMenuInput.Action.Start -> { if(event.action==KeyEvent.ACTION_UP)currentFocus?.performClick();return true }
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
