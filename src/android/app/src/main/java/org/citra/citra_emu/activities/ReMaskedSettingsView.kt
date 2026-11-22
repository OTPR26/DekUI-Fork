// SPDX-License-Identifier: GPL-2.0-or-later
package org.citra.citra_emu.activities

import android.content.Context
import android.content.res.ColorStateList
import android.content.res.Configuration
import android.graphics.drawable.GradientDrawable
import android.graphics.drawable.InsetDrawable
import android.graphics.drawable.StateListDrawable
import android.text.SpannableString
import android.text.Spanned
import android.text.style.ForegroundColorSpan
import android.view.Gravity
import android.view.View
import android.view.accessibility.AccessibilityNodeInfo
import android.widget.LinearLayout
import android.widget.ImageButton
import android.widget.ImageView
import android.widget.ScrollView
import android.widget.TextView
import androidx.appcompat.widget.SwitchCompat
import androidx.core.content.res.ResourcesCompat
import kotlin.math.roundToInt
import org.citra.citra_emu.R

/** Settings at both entry points, styled after the in-game overlay. */
internal class ReMaskedSettingsView(private val context: Context, menuSize: Int) {
    private val density = context.resources.displayMetrics.density
    private val scale = menuSize.coerceIn(50, 125) / 100f
    private val bold = ResourcesCompat.getFont(context, R.font.fira_sans_bold)
    private val regular = ResourcesCompat.getFont(context, R.font.fira_sans_regular)
    private val screenWidth = context.resources.configuration.screenWidthDp
    private val landscape = context.resources.configuration.orientation == Configuration.ORIENTATION_LANDSCAPE
    private val sideInset = if (landscape) 12 else 0
    private val contentWidth = screenWidth - 2 * sideInset
    private val sidebarWidth = if (landscape) (contentWidth * .27f).roundToInt().coerceIn(110, 186)
        else (contentWidth * .29f).roundToInt().coerceIn(90, 125)

    val content = column()
    val root = column()
    private var activeCategory: View? = null
    private var categoryList: LinearLayout? = null
    private lateinit var contentScroll: ScrollView

    companion object {
        const val ACCENT = 0xFFB97DF2.toInt()
        private const val TEXT = 0xFFF2F2F2.toInt()
        private const val MUTED = 0xFFB0A6BD.toInt()
        private const val BACKGROUND = 0xFF08070D.toInt()
        private const val PANEL = 0xFF100D17.toInt()
        private const val CONTROL = 0xFF241C30.toInt()
        private const val SELECTED = 0xFF3D1469.toInt()
        private const val FOCUSED = 0xFF4A1F75.toInt()
        private const val SEPARATOR = 0xFF403052.toInt()
    }

    fun dp(value: Int) = (value * density).roundToInt()

    fun applyInsets(left: Int, top: Int, right: Int, bottom: Int) {
        root.setPadding(dp(8) + maxOf(left, dp(sideInset)), dp(5) + top,
            dp(8) + maxOf(right, dp(sideInset)), dp(12) + maxOf(bottom, dp(if (landscape) 20 else 0)))
    }
    private fun column() = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
    private fun horizontal() = LinearLayout(context).apply { gravity = Gravity.CENTER_VERTICAL }

    private fun fill(color: Int, radius: Int = 0) = GradientDrawable().apply {
        setColor(color)
        cornerRadius = dp(radius).toFloat()
    }

    private fun rowBackground(active: Boolean, category: Boolean = false) = StateListDrawable().apply {
        addState(intArrayOf(android.R.attr.state_pressed), fill(FOCUSED, 4))
        addState(intArrayOf(android.R.attr.state_focused), fill(FOCUSED, 4).apply {
            setStroke(dp(1), ACCENT)
        })
        addState(intArrayOf(), fill(if (active) SELECTED else if (category) 0 else CONTROL, 4))
    }

    fun text(value: CharSequence, dim: Boolean = false, heading: Boolean = false) = TextView(context).apply {
        text = value
        includeFontPadding = false
        typeface = if (heading) bold else regular
        textSize = (if (dim) 13f else 16f) * scale
        setTextColor(if (heading) ACCENT else if (dim) MUTED else TEXT)
        gravity = Gravity.CENTER_VERTICAL
        setPadding(dp(14), dp(6), dp(14), dp(6))
    }

    fun choiceRow(title: String, value: String, action: () -> Unit): View =
        actionRow(title, value, "›", false, action)

    fun button(value: String, detail: String? = null, action: () -> Unit): View =
        actionRow(value, detail, "›", false, action)

    fun option(value: String, selected: Boolean, action: () -> Unit): View =
        actionRow(value, null, if (selected) "✓" else null, false, action)

    fun info(title: String, detail: String): View = labels(title, detail)

    private fun labels(title: String, detail: String?) = column().apply {
        addView(text(title))
        if (detail != null) addView(text(detail, dim = true).apply {
            setPadding(dp(14), 0, dp(14), dp(8))
        })
    }

    private fun actionRow(title: String, detail: String?, accessory: String?, active: Boolean,
                          action: () -> Unit, category: Boolean = false) =
        horizontal().apply {
            addView(labels(title, detail), LinearLayout.LayoutParams(0, -2, 1f))
            if (accessory != null) addView(text(accessory, dim = accessory == "›").apply {
                if (accessory == "✓") setTextColor(ACCENT)
            })
            minimumHeight = dp(44)
            isClickable = true
            isFocusable = true
            isSelected = active
            background = rowBackground(active, category)
            accessibilityDelegate = object : View.AccessibilityDelegate() {
                override fun onInitializeAccessibilityNodeInfo(host: View, info: AccessibilityNodeInfo) {
                    super.onInitializeAccessibilityNodeInfo(host, info)
                    info.className = android.widget.Button::class.java.name
                }
            }
            setOnClickListener { action() }
        }

    fun toggle(value: String, current: Boolean, change: (Boolean) -> Unit) = SwitchCompat(context).apply {
        text = value
        includeFontPadding = false
        textSize = 16f * scale
        typeface = regular
        setTextColor(TEXT)
        minHeight = dp(44)
        setPadding(dp(14), dp(4), dp(14), dp(4))
        isChecked = current
        isFocusable = true
        setSwitchPadding(dp(8))
        switchMinWidth = dp(51)
        thumbDrawable = InsetDrawable(fill(TEXT, 14).apply { setSize(dp(25), dp(25)) }, dp(3))
        trackDrawable = fill(PANEL, 16).apply { setSize(dp(51), dp(31)) }
        thumbTintList = ColorStateList.valueOf(TEXT)
        trackTintList = ColorStateList(
            arrayOf(intArrayOf(android.R.attr.state_checked), intArrayOf()),
            intArrayOf(ACCENT, 0xFF55505F.toInt())
        )
        background = rowBackground(false)
        setOnCheckedChangeListener { _, on -> change(on) }
    }

    fun sliderRow(title: String, caption: TextView, slider: View, reset: (() -> Unit)? = null): View = column().apply {
        background = fill(CONTROL, 4)
        addView(horizontal().apply {
            addView(text(title), LinearLayout.LayoutParams(0, -2, 1f))
            caption.gravity = Gravity.END or Gravity.CENTER_VERTICAL
            caption.setPadding(dp(5), 0, dp(14), 0)
            addView(caption)
            if (reset != null) {
                addView(ImageButton(context).apply {
                    setImageResource(R.drawable.ic_refresh)
                    imageTintList = ColorStateList.valueOf(ACCENT)
                    scaleType = ImageView.ScaleType.CENTER
                    background = rowBackground(false)
                    setPadding(0, 0, 0, 0)
                    contentDescription = "Reset $title"
                    tooltipText = contentDescription
                    isFocusable = true
                    setOnClickListener { reset() }
                }, LinearLayout.LayoutParams(dp(44), dp(44)))
            }
        })
        addView(slider, LinearLayout.LayoutParams(-1, dp(30)).apply {
            marginStart = dp(10)
            marginEnd = dp(10)
            bottomMargin = dp(4)
        })
    }

    private fun addRow(list: LinearLayout, view: View) {
        list.addView(view, LinearLayout.LayoutParams(-1, -2).apply { bottomMargin = dp(5) })
    }

    fun add(view: View) = addRow(content, view)

    fun show(categories: Collection<String>, current: String, choiceTitle: String?, inGame: Boolean,
             close: () -> Unit, select: (String) -> Unit) {
        applyInsets(0, 0, 0, 0)
        root.setBackgroundColor(BACKGROUND)
        val header = horizontal()
        val brand = "Majora ReMasked"
        val breadcrumb = SpannableString("$brand / Settings").apply {
            setSpan(ForegroundColorSpan(ACCENT), 0, brand.length, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
        }
        header.addView(text(breadcrumb).apply {
            textSize = (if (landscape) 21f else 17f) * scale
            typeface = bold
            setPadding(dp(8), 0, 0, 0)
        }, LinearLayout.LayoutParams(0, -1, 1f))
        header.addView(text(if (inGame && choiceTitle == null) "Resume" else "Back").apply {
            textSize = 15f * scale
            gravity = Gravity.CENTER
            background = rowBackground(false)
            isFocusable = true
            isClickable = true
            setOnClickListener { close() }
        }, LinearLayout.LayoutParams(dp(82), dp(36)))
        root.addView(header, LinearLayout.LayoutParams(-1, dp(48)).apply { bottomMargin = dp(7) })

        val body = horizontal()
        body.gravity = Gravity.TOP
        val rail = column()
        categoryList = rail
        categories.forEach { name ->
            val item = actionRow(name, null, null, name == current, { select(name) }, category = true)
            if (name == current) activeCategory = item
            addRow(rail, item)
        }
        fun scrolling(list: LinearLayout) = ScrollView(context).apply {
            isVerticalScrollBarEnabled = false
            addView(list)
            isFillViewport = true
        }
        body.addView(scrolling(rail).apply { background = fill(PANEL, 5) },
            LinearLayout.LayoutParams(dp(sidebarWidth), -1))
        contentScroll = scrolling(content)
        val panel = column().apply {
            background = fill(PANEL, 5)
            setPadding(dp(8), dp(4), dp(8), dp(4))
            addView(text(choiceTitle ?: current, heading = true))
            addView(View(context).apply { setBackgroundColor(SEPARATOR) },
                LinearLayout.LayoutParams(-1, dp(1).coerceAtLeast(1)).apply { bottomMargin = dp(7) })
            addView(contentScroll, LinearLayout.LayoutParams(-1, 0, 1f))
        }
        body.addView(panel, LinearLayout.LayoutParams(0, -1, 1f).apply { marginStart = dp(8) })
        root.addView(body, LinearLayout.LayoutParams(-1, 0, 1f))
    }

    val scrollPosition: Int get() = contentScroll.scrollY

    fun restoreScroll(position: Int) {
        contentScroll.post { contentScroll.scrollTo(0, position) }
    }

    private fun isWithin(child: View?, parent: View): Boolean =
        child != null && (child == parent || isWithin(child.parent as? View, parent))

    fun isCategoryFocused() = categoryList?.let { isWithin(root.findFocus(), it) } == true
    fun isContentFocused() = isWithin(root.findFocus(), content)

    fun focusFirstControl() {
        val children = arrayListOf<View>()
        content.addFocusables(children, View.FOCUS_FORWARD)
        children.firstOrNull()?.requestFocus()
    }

    fun focusCategory() {
        if (!root.isInTouchMode) activeCategory?.requestFocus()
    }
}
