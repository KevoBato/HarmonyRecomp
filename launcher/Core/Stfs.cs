using System.Buffers.Binary;
using System.Text;

namespace HarmonyLauncher.Core;

// Reads the header of an Xbox 360 LIVE/PIRS package (the same fields as
// scripts/extract_game.py), so a wrong file is rejected before a long build.
public sealed record PackageInfo(string Path, uint ContentType, uint TitleId, string DisplayName)
{
    public bool IsThisGame => TitleId == Stfs.TitleId;
    public bool IsBaseGame => IsThisGame && ContentType == Stfs.ContentTypeXbla;
    public bool IsDlc => IsThisGame && ContentType == Stfs.ContentTypeDlc;
}

public static class Stfs
{
    public const uint TitleId = 0x58410A7A;          // Castlevania: Harmony of Despair
    public const uint ContentTypeXbla = 0x000D0000;  // Arcade title (58410A7A/000D0000/)
    public const uint ContentTypeDlc = 0x00000002;   // Marketplace content (58410A7A/00000002/)

    public static PackageInfo? TryRead(string path)
    {
        try
        {
            var header = new byte[0x491];
            using var f = File.OpenRead(path);
            if (f.Read(header, 0, header.Length) != header.Length)
                return null;
            var magic = Encoding.ASCII.GetString(header, 0, 4);
            if (magic != "LIVE" && magic != "PIRS")
                return null;
            uint contentType = BinaryPrimitives.ReadUInt32BigEndian(header.AsSpan(0x344));
            uint titleId = BinaryPrimitives.ReadUInt32BigEndian(header.AsSpan(0x360));
            var name = Encoding.BigEndianUnicode.GetString(header, 0x411, 0x80);
            int nul = name.IndexOf('\0');
            return new PackageInfo(path, contentType, titleId, (nul >= 0 ? name[..nul] : name).Trim());
        }
        catch (IOException) { return null; }
        catch (UnauthorizedAccessException) { return null; }
    }
}
