// BmsFwPack — BMS firmware packaging tool (per <BMS Firmware Upgrade Communication
// Protocol Spec> ch.7/8): whole-image CRC-32/ISO-HDLC, little-endian, appended at
// the end of the firmware file (the step srec_cat does in the vendor flow).
//
// Usage:
//   BmsFwPack pack <input.bin> [output.bin] [--pad N]
//       Compute CRC32 over the (optionally 0xFF-padded) image body and append it
//       as 4 bytes little-endian. Default output: <input>_crc.bin.
//       --pad N  pad the body with 0xFF up to exactly N bytes before the CRC.
//   BmsFwPack check <input.bin>
//       Verify that the trailing 4 bytes equal CRC32 of everything before them.
//
// Examples:
//   BmsFwPack pack CS-L10-6K2_1.1.0.bin
//   BmsFwPack pack CS-L10-6K2_1.1.0.bin out.bin --pad 92156
//   BmsFwPack check CS-L10-6K2_1.1.0.bin
//
// Build: csc /nologo /optimize BmsFwPack.cs

using System;
using System.IO;
using System.Security.Cryptography;

static class BmsFwPack
{
    static uint[] _table;

    static void BuildTable()
    {
        _table = new uint[256];
        for (uint i = 0; i < 256; i++)
        {
            uint c = i;
            for (int k = 0; k < 8; k++)
                c = ((c & 1) != 0) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            _table[i] = c;
        }
    }

    // CRC-32/ISO-HDLC (same as zlib.crc32 / srec_cat -crc32): init 0xFFFFFFFF,
    // reflected poly 0xEDB88320, final xor 0xFFFFFFFF.
    static uint Crc32(byte[] data, int length)
    {
        uint crc = 0xFFFFFFFFu;
        for (int i = 0; i < length; i++)
            crc = _table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
        return crc ^ 0xFFFFFFFFu;
    }

    static string Sha256(byte[] data)
    {
        using (var sha = SHA256.Create())
        {
            var sb = new System.Text.StringBuilder();
            foreach (byte b in sha.ComputeHash(data))
                sb.Append(b.ToString("x2"));
            return sb.ToString();
        }
    }

    static int Main(string[] args)
    {
        if (args.Length < 2)
        {
            Console.WriteLine("BmsFwPack - BMS firmware CRC packaging tool");
            Console.WriteLine("  pack  <input.bin> [output.bin] [--pad N] : append CRC32 (LE) at tail");
            Console.WriteLine("  check <input.bin>                        : verify trailing CRC32");
            return 2;
        }

        BuildTable();
        string mode = args[0].ToLowerInvariant();

        try
        {
            if (mode == "check") return DoCheck(args[1]);
            if (mode == "pack") return DoPack(args);
            Console.WriteLine("unknown mode: " + mode);
            return 2;
        }
        catch (Exception ex)
        {
            Console.WriteLine("ERROR: " + ex.Message);
            return 1;
        }
    }

    static byte[] ReadAll(string path)
    {
        if (!File.Exists(path))
            throw new FileNotFoundException("input not found: " + path);
        return File.ReadAllBytes(path);
    }

    static int DoCheck(string input)
    {
        byte[] data = ReadAll(input);
        if (data.Length < 4)
        {
            Console.WriteLine("FAIL: file too small ({0} bytes)", data.Length);
            return 1;
        }
        uint stored = BitConverter.ToUInt32(data, data.Length - 4);
        uint computed = Crc32(data, data.Length - 4);
        Console.WriteLine("file     : {0}", Path.GetFullPath(input));
        Console.WriteLine("size     : {0}", data.Length);
        Console.WriteLine("stored   : CRC32 = 0x{0:X8}", stored);
        Console.WriteLine("computed : CRC32 over first {0} bytes = 0x{1:X8}", data.Length - 4, computed);
        bool ok = stored == computed;
        Console.WriteLine(ok ? "RESULT   : OK (trailing CRC32 matches)"
                            : "RESULT   : FAIL (no valid trailing CRC32 - raw/unpackaged build?)");
        Console.WriteLine("sha256   : {0}", Sha256(data));
        return ok ? 0 : 1;
    }

    static int DoPack(string[] args)
    {
        string input = args[1];
        string output = null;
        long padTo = -1;

        for (int i = 2; i < args.Length; i++)
        {
            string a = args[i];
            if (a == "--pad" && i + 1 < args.Length)
            {
                if (!long.TryParse(args[++i], out padTo) || padTo < 0)
                    throw new ArgumentException("--pad needs a byte count");
            }
            else if (output == null)
            {
                output = a;
            }
            else
            {
                throw new ArgumentException("too many arguments: " + a);
            }
        }
        if (output == null)
        {
            string ext = Path.GetExtension(input);
            output = input.Substring(0, input.Length - ext.Length) + "_crc.bin";
        }

        byte[] body = ReadAll(input);
        int bodyLen = body.Length;
        long total = (long)bodyLen + 4;
        if (padTo >= 0)
        {
            if (padTo < bodyLen)
                throw new ArgumentException(string.Format(
                    "--pad {0} is smaller than input ({1} bytes)", padTo, bodyLen));
            total = padTo + 4;
        }

        byte[] outBuf = new byte[total];
        Array.Copy(body, outBuf, bodyLen);           // 0xFF not guaranteed by Array.Clear;
        for (long i = bodyLen; i < total - 4; i++)   // protocol expects erased-state fill
            outBuf[i] = 0xFF;
        uint crc = Crc32(outBuf, (int)(total - 4));
        outBuf[total - 4] = (byte)(crc & 0xFF);
        outBuf[total - 3] = (byte)((crc >> 8) & 0xFF);
        outBuf[total - 2] = (byte)((crc >> 16) & 0xFF);
        outBuf[total - 1] = (byte)((crc >> 24) & 0xFF);

        File.WriteAllBytes(output, outBuf);
        Console.WriteLine("input    : {0} ({1} bytes)", Path.GetFullPath(input), bodyLen);
        if (padTo >= 0 && padTo != bodyLen)
            Console.WriteLine("padding  : 0xFF filled to {0} bytes (+{1})", padTo, padTo - bodyLen);
        Console.WriteLine("CRC32    : 0x{0:X8} (over first {1} bytes)", crc, total - 4);
        Console.WriteLine("output   : {0} ({1} bytes)", Path.GetFullPath(output), total);
        Console.WriteLine("sha256   : {0}", Sha256(outBuf));
        Console.WriteLine("blocks   : transport {0} B -> PackSum = {1}",
                          total + 16, (total + 16 - 16 + 1023) / 1024);
        return 0;
    }
}
