from unihiker_k10 import screen
import time


def load_pbm(filename, offset_x=0, offset_y=0):
    with open(filename, 'rb') as f:
        # Read magic number
        magic = f.readline().decode('ascii').strip()
        if magic !=  'P4':
            raise ValueError("Not a valid PBM file")

        # Skip comments
        line = f.readline().decode('ascii')
        while line.startswith('#'):
            line = f.readline().decode('ascii')

        # Read width and height
        width, height = map(int, line.strip().split())

        data = f.read()
        pixel_index = 0
        bytes_per_row = (width + 7) // 8
        for row in range(height):
            byte_index = row * bytes_per_row
            for col in range(0, width, 8):
                byte_val = data[byte_index]
                bits_to_process = min(8, width - col)
                for bit in range(bits_to_process):
                    bit_val = (byte_val >> (7 - bit)) & 1
                    color = 0xFFFFFF if bit_val == 0 else 0x000000
                    screen.draw_point(x=offset_x + col + bit, y=offset_y + row, color=color)
                    pixel_index += 1
                byte_index += 1
            screen.show_draw()


screen.init(dir=2)
screen.show_bg(color=0xFFFF00)

# Load and draw PBM image
load_pbm('wind-fox.pbm')

screen.show_draw()

while True:
    time.sleep(1)