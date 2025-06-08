from PIL import Image
import sys

im = Image.open(sys.argv[1])

output = open(sys.argv[2], 'wb')

for x in range(im.width):
    for y in range(im.height):
        px = im.getpixel((x, y))
        #print(px)
        #print((px[0] >> 6, px[1] >> 6, px[2] >> 6))
        # blue green red (msb to lsb)
        color_conv = (px[0] >> 6) | ((px[1] >> 6) << 2) | ((px[2] >> 6) << 4)
        print(hex(color_conv))
        output.write(color_conv.to_bytes(1, 'little'))


output.close()
