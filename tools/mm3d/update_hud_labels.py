"""Replace the two legacy item labels in the fork's existing HUD atlas."""
from pathlib import Path
from PIL import Image, ImageDraw


def replace_labels(image):
    if image.size != (4096, 2048):
        raise ValueError('Expected the 8x HUD atlas')
    result = image.convert('RGBA').copy()
    outlines = (
        [(36,18),(53,18),(53,89),(94,89),(94,106),(36,106)],
        [(31,18),(70,18),(91,31),(91,61),(76,73),(99,106),(77,106),
         (55,77),(48,77),(48,106),(31,106)],
    )
    for index, points in enumerate(outlines):
        glyph = Image.new('RGBA', (128, 128))
        draw = ImageDraw.Draw(glyph)
        draw.polygon(points, fill=(255, 240, 0, 255))
        draw.line(points + [points[0]], fill='black', width=8, joint='curve')
        if index == 1:
            hole = [(48,35),(65,35),(74,41),(74,53),(65,59),(48,59)]
            draw.polygon(hole, fill=(0, 0, 0, 0))
            draw.line(hole + [hole[0]], fill='black', width=7, joint='curve')
        result.paste(glyph, ((480 + index * 16) * 8, 80 * 8))
    return result


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('atlas', type=Path)
    args = parser.parse_args()
    replace_labels(Image.open(args.atlas)).save(args.atlas)
