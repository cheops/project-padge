"""
Generates badge_manual_a4.pdf: an A4 sheet with 4x English cards on page 1
and 4x Dutch cards on page 2, each sized to exactly 1/4 of A4 (= A6), meant
to be cut into 4 handout cards.

Usage:
    pip install -r requirements.txt
    python make_badge_manual.py

Requires the Noto Color Emoji font to be installed on the system (used to
render the 3 colored circle icons as PNGs before placing them in the PDF --
reportlab cannot render color emoji glyphs directly from a font).

On Debian/Ubuntu:
    sudo apt-get install fonts-noto-color-emoji
On macOS:
    brew install --cask font-noto-color-emoji
    (or substitute any other color emoji font file you have and update
    EMOJI_FONT_PATH below)
"""

import os
from PIL import Image, ImageDraw, ImageFont
from reportlab.lib.pagesizes import A4
from reportlab.pdfgen import canvas
from reportlab.pdfbase.pdfmetrics import stringWidth

# ------------------------------------------------------------------------
# Config
# ------------------------------------------------------------------------

OUTPUT_PDF = "badge_manual_a4.pdf"
ASSETS_DIR = "assets"

# Adjust this path if your color emoji font lives elsewhere.
EMOJI_FONT_PATH = "/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf"
EMOJI_RENDER_SIZE = 109  # px, matches NotoColorEmoji's native bitmap strike

PAGE_W, PAGE_H = A4
CELL_W, CELL_H = PAGE_W / 2, PAGE_H / 2
MARGIN = 20  # pt

ICONS = {
    "green": os.path.join(ASSETS_DIR, "emoji_green.png"),
    "red": os.path.join(ASSETS_DIR, "emoji_red.png"),
    "blue": os.path.join(ASSETS_DIR, "emoji_blue.png"),
}

EMOJI_CHARS = {
    "green": "\U0001F7E2",  # 🟢
    "red": "\U0001F534",    # 🔴
    "blue": "\U0001F535",   # 🔵
}

STYLES = {
    "title":   ("Helvetica-Bold", 15,   17,   10),
    "heading": ("Helvetica-Bold", 10.5, 12.5, 4),
    "body":    ("Helvetica",      9,    10.6, 6.5),
}


# ------------------------------------------------------------------------
# Step 1: render the color emoji icons to transparent PNGs
# ------------------------------------------------------------------------

def render_emoji_icons():
    os.makedirs(ASSETS_DIR, exist_ok=True)
    font = ImageFont.truetype(EMOJI_FONT_PATH, EMOJI_RENDER_SIZE)
    for name, ch in EMOJI_CHARS.items():
        path = ICONS[name]
        if os.path.exists(path):
            continue
        img = Image.new("RGBA", (136, 136), (0, 0, 0, 0))
        draw = ImageDraw.Draw(img)
        draw.text((0, 0), ch, font=font, embedded_color=True)
        img.save(path)


# ------------------------------------------------------------------------
# Step 2: text layout helpers
# ------------------------------------------------------------------------

def wrap_text(text, font, size, max_width):
    words = text.split(" ")
    lines, cur = [], ""
    for w in words:
        test = (cur + " " + w).strip()
        if stringWidth(test, font, size) <= max_width:
            cur = test
        else:
            if cur:
                lines.append(cur)
            cur = w
    if cur:
        lines.append(cur)
    return lines


def draw_paragraph(c, x, y_top, max_width, text, font, size, leading, icon=None):
    """Draws a (possibly multi-line, \\n-separated) paragraph starting at
    y_top. If icon is given, indents every line to make room for it and
    draws the icon once, vertically centered on the first line."""
    icon_w = leading if icon else 0
    icon_gap = 4 if icon else 0
    indent = icon_w + icon_gap

    y = y_top
    first_line = True
    for para in text.split("\n"):
        lines = wrap_text(para, font, size, max_width - indent)
        for line in lines:
            c.setFont(font, size)
            c.drawString(x + indent, y, line)
            if icon and first_line:
                c.drawImage(
                    ICONS[icon],
                    x, y - size * 0.22,
                    width=icon_w, height=icon_w,
                    mask="auto", preserveAspectRatio=True,
                )
            first_line = False
            y -= leading
    return y


def draw_block(c, x, y_top, max_width, block, font, size, leading):
    icon = block[2] if len(block) > 2 else None
    return draw_paragraph(c, x, y_top, max_width, block[1], font, size, leading, icon)


def draw_card(c, ox, oy, content):
    x = ox + MARGIN
    max_w = CELL_W - 2 * MARGIN
    y = oy + CELL_H - MARGIN - 9

    for block in content:
        kind = block[0]
        font, size, leading, gap_after = STYLES[kind]
        y = draw_block(c, x, y, max_w, block, font, size, leading)
        y -= gap_after

    c.saveState()
    c.setDash(2, 2)
    c.setLineWidth(0.4)
    c.setStrokeGray(0.6)
    c.rect(ox + 3, oy + 3, CELL_W - 6, CELL_H - 6)
    c.restoreState()


def build_page(c, content):
    positions = [
        (0, PAGE_H / 2),
        (PAGE_W / 2, PAGE_H / 2),
        (0, 0),
        (PAGE_W / 2, 0),
    ]
    for ox, oy in positions:
        draw_card(c, ox, oy, content)

    c.saveState()
    c.setDash(3, 3)
    c.setLineWidth(0.5)
    c.setStrokeGray(0.4)
    c.line(0, PAGE_H / 2, PAGE_W, PAGE_H / 2)
    c.line(PAGE_W / 2, 0, PAGE_W / 2, PAGE_H)
    c.restoreState()


# ------------------------------------------------------------------------
# Step 3: card content
# ------------------------------------------------------------------------

EN = [
    ("title", "Your Fox Badge - Quick Guide"),
    ("body", "Lights up on its own when it senses motion, or with a quick "
             "button press. Sleeps the rest of the time to save battery."),
    ("heading", "The Button"),
    ("body", "- Quick press: plays the next light animation (7 to cycle "
             "through)\n- Hold about 2 seconds: switches mode, flashes "
             "green or red to confirm"),
    ("heading", "Two Modes"),
    ("body", "Motion mode: lights up on motion or on a button press.", "green"),
    ("body", "Manual mode: lights up only on a button press - saves "
             "battery when not worn.", "red"),
    ("heading", "Battery"),
    ("body", "Two CR2032 coin cell batteries. Dim or unresponsive lights "
             "usually mean it is time to replace them."),
    ("body", "Repeated blue flashing instead of an animation is an "
             "internal error signal. The button still works as a plain "
             "light, but please get in touch so we can help.", "blue"),
    ("heading", "Tip"),
    ("body", "No power switch needed: Manual mode plus not touching the "
             "button is effectively off, and it uses almost no power "
             "while asleep."),
]

NL = [
    ("title", "Je Fox Badge - Snelle Gids"),
    ("body", "Gaat vanzelf branden zodra hij beweging voelt, of met een "
             "korte druk op de knop. De rest van de tijd slaapt hij om de "
             "batterij te sparen."),
    ("heading", "De Knop"),
    ("body", "- Korte druk: speelt de volgende lichtanimatie af (7 om "
             "doorheen te lopen)\n- Ongeveer 2 seconden ingedrukt houden: "
             "wisselt van modus, knippert groen of rood ter bevestiging"),
    ("heading", "Twee Modi"),
    ("body", "Bewegingsmodus: licht op bij beweging of bij een druk op de "
             "knop.", "green"),
    ("body", "Handmatige modus: licht alleen op bij een druk op de knop - "
             "spaart batterij wanneer niet gedragen.", "red"),
    ("heading", "Batterij"),
    ("body", "Twee CR2032 knoopcelbatterijen. Dof of niet reagerend licht "
             "betekent meestal dat het tijd is om ze te vervangen."),
    ("body", "Herhaaldelijk blauw knipperen in plaats van een animatie is "
             "een intern foutsignaal. De knop werkt dan nog wel als "
             "gewone lichtbadge, maar neem contact met ons op zodat we "
             "kunnen helpen.", "blue"),
    ("heading", "Tip"),
    ("body", "Geen aan/uit-knop nodig: Handmatige modus zonder de knop "
             "aan te raken is in feite uit, en hij gebruikt bijna geen "
             "stroom tijdens de slaap."),
]


# ------------------------------------------------------------------------
# Main
# ------------------------------------------------------------------------

def main():
    render_emoji_icons()

    c = canvas.Canvas(OUTPUT_PDF, pagesize=A4)
    build_page(c, EN)
    c.showPage()
    build_page(c, NL)
    c.showPage()
    c.save()
    print(f"Wrote {OUTPUT_PDF}")


if __name__ == "__main__":
    main()
