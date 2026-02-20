from unihiker_k10 import screen
import bluetooth
import struct
import uasyncio as asyncio  # MicroPython's asyncio
import time


screen.init(dir=2)

# Ruuvi constants
RUUVI_COMPANY_ID:int = 0x0499
RUUVI_FORMAT = 5  # Data Format 5 (RAWv2)

# Function to parse Ruuvi Data Format 5
def parse_ruuvitag_data(data):
    if len(data) != 24:
        return None

    # Unpack all 24 bytes: format (B), temp (h), hum (H), press (H), ax (h), ay (h), az (h), power (H), mov (B), seq (H), mac (6s)
    format_id, temp_raw, hum_raw, press_raw, accel_x_raw, accel_y_raw, accel_z_raw, power_raw, movement, seq, mac = struct.unpack(
        '>BhHHhhhHBH6s', data
    )

    if format_id != RUUVI_FORMAT:
        return None  # Not Data Format 5

    # Temperature: int16 * 0.005 °C
    temperature = None if temp_raw == -32768 else temp_raw * 0.005

    # Humidity: uint16 * 0.0025 %
    humidity = None if hum_raw == 65535 else hum_raw * 0.0025

    # Pressure: uint16 (value = (hPa * 100) - 50000 Pa)
    pressure = None if press_raw == 0 else (press_raw + 50000) / 100.0

    # Acceleration: int16 milli-g, /1000 for g
    accel_x = None if accel_x_raw == -32768 else accel_x_raw / 1000.0
    accel_y = None if accel_y_raw == -32768 else accel_y_raw / 1000.0
    accel_z = None if accel_z_raw == -32768 else accel_z_raw / 1000.0

    # Power info: uint16 (bits 0-4: TX, bits 5-15: battery mV - 1600)
    tx_bits = power_raw & 0b00011111  # Bits 0-4
    tx_power = None if tx_bits == 0b11111 else (tx_bits * 2) - 40

    battery_bits = power_raw >> 5  # Bits 5-15 (11 bits)
    battery_mv = None if battery_bits == 0b11111111111 else battery_bits + 1600
    battery_voltage = None if battery_mv is None else battery_mv / 1000.0

    # Movement and sequence
    movement_counter = None if movement == 255 else movement
    sequence = None if seq == 65535 else seq

    return {
        'temperature': temperature,
        'humidity': humidity,
        'pressure': pressure,
        'battery_voltage': battery_voltage,
        'tx_power': tx_power,
        'sequence': sequence,
    }

# BLE scanner class
class BLEScanner:
    def __init__(self):
        self._ble = bluetooth.BLE()
        self._ble.active(True)
        self._ble.irq(self._irq)

    def _irq(self, event, data):
        if event == 5:  # Scan result
            addr_type, addr, adv_type, rssi, adv_data = data
            # Look for manufacturer data (type 0xFF)
            i = 0
            while i < len(adv_data)-4:
                length = adv_data[i]
                ad_type = adv_data[i+1]
                if ad_type == 0xFF:  # Manufacturer specific
                    company_id = adv_data[i+2] + (adv_data[i+3] << 8)
                    if company_id == RUUVI_COMPANY_ID:
                        ruuvi_data = adv_data[i+4:i+2+length]
                        parsed = parse_ruuvitag_data(ruuvi_data)
                        if parsed:
                            print("Ruuvi Data:", parsed)
                i += length + 1

    async def scan(self, duration_ms=10000):
        self._ble.gap_scan(duration_ms, 30000, 30000)  # Scan for 10s
        await asyncio.sleep_ms(duration_ms)

# Initialize scanner
scanner = BLEScanner()

# Main loop with async scanning
async def main_loop():
    while True:
        await asyncio.sleep(1)  # Existing delay
        await scanner.scan(10000)  # Scan every ~10s

# Run the async loop
loop = asyncio.get_event_loop()
loop.run_until_complete(main_loop())