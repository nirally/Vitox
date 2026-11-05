// Vitox Xbox harness - native helpers (compiled by VitoxXbox.ps1 via Add-Type).
// TLS thumbprint pinning, PE/PDB debug-id parsing, package publisher-id hash,
// and the Device Portal multipart upload body. Vitox-only.
using System;
using System.Collections.Generic;
using System.IO;
using System.Net;
using System.Net.Http;
using System.Net.Security;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using System.Threading.Tasks;

namespace VitoxHarness
{
    public static class Tls
    {
        // SHA-256 of the DER certificate, upper-case hex, no separators. null = nothing pinned.
        public static string Pinned;
        public static string LastSeenSha256;
        public static string LastSubject;
        public static string LastIssuer;
        public static string LastNotBefore;
        public static string LastNotAfter;
        public static string LastPolicyErrors;

        public static bool Check(X509Certificate2 cert, SslPolicyErrors errors)
        {
            if (cert == null) { LastSeenSha256 = null; return false; }
            LastSeenSha256 = cert.GetCertHashString(HashAlgorithmName.SHA256).ToUpperInvariant();
            LastSubject = cert.Subject;
            LastIssuer = cert.Issuer;
            LastNotBefore = cert.NotBefore.ToUniversalTime().ToString("o");
            LastNotAfter = cert.NotAfter.ToUniversalTime().ToString("o");
            LastPolicyErrors = errors.ToString();
            // Trust is decided only by the pinned thumbprint (self-signed console cert).
            return !string.IsNullOrEmpty(Pinned) &&
                   string.Equals(LastSeenSha256, Pinned, StringComparison.OrdinalIgnoreCase);
        }

        static bool Validate(HttpRequestMessage m, X509Certificate2 c, X509Chain ch, SslPolicyErrors e)
        {
            return Check(c, e);
        }

        public static HttpClient CreateClient(string pinned, int timeoutSeconds, CookieContainer cookies)
        {
            Pinned = string.IsNullOrEmpty(pinned) ? null : pinned.ToUpperInvariant();
            LastSeenSha256 = null;
            var handler = new HttpClientHandler
            {
                // Cookies are handled explicitly by the harness (CookieContainer drops the console's CSRF-Token cookie).
                UseCookies = false,
                AllowAutoRedirect = false,
                ServerCertificateCustomValidationCallback = Validate,
            };
            return new HttpClient(handler) { Timeout = TimeSpan.FromSeconds(timeoutSeconds) };
        }
    }

    public static class DebugId
    {
        // Returns { GUID, Age, PdbPath } from the CodeView RSDS record of a PE image.
        public static string[] ReadPeRsds(byte[] pe)
        {
            int lfanew = BitConverter.ToInt32(pe, 0x3C);
            if (BitConverter.ToUInt32(pe, lfanew) != 0x00004550) throw new InvalidDataException("not a PE image");
            int coff = lfanew + 4;
            int nsect = BitConverter.ToUInt16(pe, coff + 2);
            int optSize = BitConverter.ToUInt16(pe, coff + 16);
            int opt = coff + 20;
            ushort magic = BitConverter.ToUInt16(pe, opt);
            int ddBase = opt + (magic == 0x20B ? 112 : 96);
            uint dbgRva = BitConverter.ToUInt32(pe, ddBase + 6 * 8);
            uint dbgSize = BitConverter.ToUInt32(pe, ddBase + 6 * 8 + 4);
            int sect = opt + optSize;
            long dbgOff = -1;
            for (int i = 0; i < nsect; i++)
            {
                int s = sect + i * 40;
                uint va = BitConverter.ToUInt32(pe, s + 12), vsz = BitConverter.ToUInt32(pe, s + 8);
                uint raw = BitConverter.ToUInt32(pe, s + 20);
                if (dbgRva >= va && dbgRva < va + Math.Max(vsz, BitConverter.ToUInt32(pe, s + 16))) { dbgOff = raw + (dbgRva - va); break; }
            }
            if (dbgOff < 0) throw new InvalidDataException("debug directory not found");
            for (long e = dbgOff; e < dbgOff + dbgSize; e += 28)
            {
                if (BitConverter.ToUInt32(pe, (int)e + 12) != 2) continue; // IMAGE_DEBUG_TYPE_CODEVIEW
                int p = (int)BitConverter.ToUInt32(pe, (int)e + 24);
                if (Encoding.ASCII.GetString(pe, p, 4) != "RSDS") continue;
                var g = new byte[16];
                Array.Copy(pe, p + 4, g, 0, 16);
                uint age = BitConverter.ToUInt32(pe, p + 20);
                int end = p + 24; while (pe[end] != 0) end++;
                return new[] { new Guid(g).ToString("D").ToUpperInvariant(), age.ToString(), Encoding.UTF8.GetString(pe, p + 24, end - p - 24) };
            }
            throw new InvalidDataException("no RSDS record");
        }

        // Returns { GUID, Age } from the PDB info stream (stream 1) of an MSF 7.00 PDB.
        public static string[] ReadPdbInfo(string path)
        {
            using (var f = File.OpenRead(path))
            using (var r = new BinaryReader(f))
            {
                var magic = Encoding.ASCII.GetString(r.ReadBytes(32));
                if (!magic.StartsWith("Microsoft C/C++ MSF 7.00")) throw new InvalidDataException("not an MSF 7.00 PDB");
                uint blockSize = r.ReadUInt32(); r.ReadUInt32(); r.ReadUInt32();
                uint dirBytes = r.ReadUInt32(); r.ReadUInt32();
                uint blockMapAddr = r.ReadUInt32();
                uint dirBlocks = (dirBytes + blockSize - 1) / blockSize;
                f.Position = (long)blockMapAddr * blockSize;
                var dirBlockList = new uint[dirBlocks];
                for (int i = 0; i < dirBlocks; i++) dirBlockList[i] = r.ReadUInt32();
                var dir = new MemoryStream();
                foreach (var b in dirBlockList) { f.Position = (long)b * blockSize; dir.Write(r.ReadBytes((int)blockSize), 0, (int)blockSize); }
                dir.Position = 0;
                var dr = new BinaryReader(dir);
                uint nStreams = dr.ReadUInt32();
                var sizes = new uint[nStreams];
                for (int i = 0; i < nStreams; i++) sizes[i] = dr.ReadUInt32();
                // Skip stream 0's block list, then read stream 1's first block.
                uint s0 = sizes[0] == 0xFFFFFFFF ? 0 : (sizes[0] + blockSize - 1) / blockSize;
                for (int i = 0; i < s0; i++) dr.ReadUInt32();
                uint firstBlock = dr.ReadUInt32();
                f.Position = (long)firstBlock * blockSize;
                r.ReadUInt32(); r.ReadUInt32(); // version, signature
                uint age = r.ReadUInt32();
                var g = r.ReadBytes(16);
                return new[] { new Guid(g).ToString("D").ToUpperInvariant(), age.ToString() };
            }
        }
    }

    public static class PackageId
    {
        // Package publisher id: first 8 bytes of SHA-256(UTF-16LE publisher), base32 (Crockford-like alphabet), 13 chars.
        public static string PublisherId(string publisher)
        {
            const string alphabet = "0123456789abcdefghjkmnpqrstvwxyz";
            byte[] h;
            using (var sha = SHA256.Create()) h = sha.ComputeHash(Encoding.Unicode.GetBytes(publisher));
            ulong v = 0;
            for (int i = 0; i < 8; i++) v = (v << 8) | h[i];
            var sb = new StringBuilder();
            // 64 bits + 1 padding zero bit = 65 bits = 13 x 5-bit groups.
            for (int i = 0; i < 13; i++)
            {
                int shift = 59 - 5 * i; // group i of (v << 1) == (v >> (59 - 5i)) & 31
                ulong group = shift >= 0 ? (v >> shift) & 0x1F : (v << -shift) & 0x1F;
                sb.Append(alphabet[(int)group]);
            }
            return sb.ToString();
        }
    }

    // Same wire format as microsoft/WindowsDevicePortalWrapper HttpMultipartFileContent.
    public sealed class MultipartFiles : HttpContent
    {
        readonly List<string> items;
        readonly string boundary = Guid.NewGuid().ToString();

        public MultipartFiles(IEnumerable<string> files)
        {
            items = new List<string>(files);
            Headers.TryAddWithoutValidation("Content-Type", "multipart/form-data; boundary=" + boundary);
        }

        static byte[] Header(FileInfo info)
        {
            string type = info.Extension.ToLowerInvariant() == ".cer" ? "application/x-x509-ca-cert" : "application/octet-stream";
            return Encoding.ASCII.GetBytes(string.Format(
                "Content-Disposition: form-data; name=\"{0}\"; filename=\"{0}\"\r\nContent-Type: {1}\r\n\r\n", info.Name, type));
        }

        protected override async Task SerializeToStreamAsync(Stream s, TransportContext c)
        {
            var b = Encoding.ASCII.GetBytes("--" + boundary + "\r\n");
            var nl = Encoding.ASCII.GetBytes("\r\n");
            foreach (var item in items)
            {
                await s.WriteAsync(b, 0, b.Length);
                var h = Header(new FileInfo(item));
                await s.WriteAsync(h, 0, h.Length);
                using (var f = File.OpenRead(item)) await f.CopyToAsync(s);
                await s.WriteAsync(nl, 0, nl.Length);
            }
            var end = Encoding.ASCII.GetBytes("--" + boundary + "--\r\n");
            await s.WriteAsync(end, 0, end.Length);
        }

        protected override bool TryComputeLength(out long length)
        {
            long bl = Encoding.ASCII.GetByteCount("--" + boundary + "\r\n");
            length = 0;
            foreach (var item in items)
            {
                var fi = new FileInfo(item);
                length += bl + Header(fi).Length + fi.Length + 2;
            }
            length += bl + 2;
            return true;
        }
    }

    // Selftest only: loopback TLS endpoint presenting a given certificate. Counts handshakes and
    // application bytes received, to prove a pin mismatch sends nothing past the handshake.
    public sealed class SelftestTlsServer : IDisposable
    {
        readonly System.Net.Sockets.TcpListener listener;
        readonly X509Certificate2 cert;
        public readonly int Port;
        public int Connections;
        public int Handshakes;
        public long AppBytesReceived;

        public SelftestTlsServer(X509Certificate2 certificate)
        {
            cert = certificate;
            listener = new System.Net.Sockets.TcpListener(IPAddress.Loopback, 0);
            listener.Start();
            Port = ((IPEndPoint)listener.LocalEndpoint).Port;
            new System.Threading.Thread(Loop) { IsBackground = true }.Start();
        }

        void Loop()
        {
            while (true)
            {
                System.Net.Sockets.TcpClient c;
                try { c = listener.AcceptTcpClient(); } catch { return; }
                System.Threading.Interlocked.Increment(ref Connections);
                try
                {
                    using (c)
                    using (var s = new SslStream(c.GetStream(), false))
                    {
                        s.ReadTimeout = 5000;
                        s.AuthenticateAsServer(cert, false, false);
                        System.Threading.Interlocked.Increment(ref Handshakes);
                        var buf = new byte[4096];
                        int n;
                        while ((n = s.Read(buf, 0, buf.Length)) > 0) System.Threading.Interlocked.Add(ref AppBytesReceived, n);
                    }
                }
                catch { }
            }
        }

        public void Dispose() { try { listener.Stop(); } catch { } }
    }
}
