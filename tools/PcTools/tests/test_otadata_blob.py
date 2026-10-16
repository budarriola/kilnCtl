import struct
import unittest

from kilnctrl import otadata_blob as ob


def rom_crc32_le(crc, data):
    # bitwise definition of the ROM crc32_le used by bootloader_common_ota_select_crc
    crc = ~crc & 0xFFFFFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ (0xEDB88320 if crc & 1 else 0)
    return ~crc & 0xFFFFFFFF


class OtadataBlobTests(unittest.TestCase):
    def test_crc_matches_rom_definition(self):
        for seq in (1, 2, 7, 0x12345678):
            self.assertEqual(ob.ota_select_crc(seq), rom_crc32_le(0xFFFFFFFF, struct.pack("<I", seq)))

    def test_known_vector_seq1(self):
        self.assertEqual(ob.ota_select_crc(1), 0x4743989A)

    def test_entry_layout(self):
        e = ob.build_entry(1)
        self.assertEqual(len(e), 32)
        seq, label, state, crc = struct.unpack("<I20sII", e)
        self.assertEqual((seq, label, state), (1, b"\xff" * 20, 0xFFFFFFFF))
        self.assertEqual(crc, rom_crc32_le(0xFFFFFFFF, e[:4]))

    def test_image_has_two_valid_sectors(self):
        img = ob.build_otadata_image(0x2000)
        self.assertEqual(len(img), 0x2000)
        self.assertEqual(img[0:32], img[0x1000:0x1020])
        self.assertEqual(img[32:0x1000], b"\xff" * (0x1000 - 32))
        # ota_seq-1 % 1 app slot == 0 -> ota_0
        self.assertEqual((struct.unpack("<I", img[:4])[0] - 1) % 1, 0)

    def test_rejects_bad_input(self):
        with self.assertRaises(ValueError):
            ob.build_entry(0)
        with self.assertRaises(ValueError):
            ob.build_otadata_image(0x1000)

    def test_matches_idf_otatool_if_present(self):
        import binascii, os
        p = os.path.join(os.environ.get("IDF_PATH", ""), "components", "app_update", "otatool.py")
        if not os.path.isfile(p):
            self.skipTest("IDF otatool.py not present")
        src = open(p, encoding="utf-8").read()
        self.assertIn("binascii.crc32(struct.pack('I', seq), 0xFFFFFFFF)", src)
        self.assertEqual(ob.ota_select_crc(5), binascii.crc32(struct.pack("I", 5), 0xFFFFFFFF) & 0xFFFFFFFF)


if __name__ == "__main__":
    unittest.main()
