// Saints Row PC Setup
// Builds Saints Row PC on the player's machine from their own Xbox 360 disc image.
// It installs the build tools it needs (Git, Visual Studio 2022 Build Tools with
// the C++ and Clang components, the Visual C++ runtime), downloads the source
// code from GitHub and runs scripts\setup.ps1. No game code is included.
//
// Build: installer\build.ps1 (uses the C# compiler that ships with Windows).

using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.IO.Compression;
using System.Net;
using System.Reflection;
using System.Text.RegularExpressions;
using System.Threading;
using System.Windows.Forms;
using Microsoft.Win32;

[assembly: AssemblyTitle("Saints Row PC Setup")]
[assembly: AssemblyProduct("Saints Row PC")]
[assembly: AssemblyCompany("whompay")]
[assembly: AssemblyVersion("1.0.0.0")]
[assembly: AssemblyFileVersion("1.0.0.0")]

namespace SaintsRowPCSetup
{
    static class Config
    {
        public const string Version = "1.0.0";
        public const string RepoUrl = "https://github.com/whompay/SaintsRowPC.git";
        public const string RepoPage = "https://github.com/whompay/SaintsRowPC";
        public const string Branch = "main";
        public const string MinGitUrl =
            "https://github.com/git-for-windows/git/releases/download/v2.47.1.windows.1/MinGit-2.47.1-64-bit.zip";
        public const string BuildToolsUrl = "https://aka.ms/vs/17/release/vs_BuildTools.exe";
        public const string VcRedistUrl = "https://aka.ms/vs/17/release/vc_redist.x64.exe";
        public static readonly string[] VsComponents = {
            "Microsoft.VisualStudio.Workload.VCTools",
            "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
            "Microsoft.VisualStudio.Component.VC.CMake.Project",
            "Microsoft.VisualStudio.Component.VC.Llvm.Clang",
            "Microsoft.VisualStudio.Component.VC.Llvm.ClangToolset",
            "Microsoft.VisualStudio.Component.Windows11SDK.22621",
        };
        public const long NeededFreeBytes = 25L * 1024 * 1024 * 1024;
    }

    static class Program
    {
        [STAThread]
        static void Main()
        {
            try { ServicePointManager.SecurityProtocol = (SecurityProtocolType)3072; } catch { } // TLS 1.2
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new SetupForm());
        }
    }

    class StepFailed : Exception { public StepFailed(string m) : base(m) { } }

    class SetupForm : Form
    {
        TextBox isoBox, dirBox, logBox;
        Button isoBrowse, dirBrowse, installBtn, cancelBtn, playBtn;
        CheckBox desktopShortcut;
        Label statusLabel;
        ProgressBar progress;
        Thread worker;
        volatile bool cancelled;
        Process current;
        StreamWriter logFile;
        readonly object logLock = new object();
        string gitDir;

        public SetupForm()
        {
            Text = "Saints Row PC Setup " + Config.Version;
            AutoScaleMode = AutoScaleMode.Dpi;
            ClientSize = new Size(720, 540);
            MinimumSize = new Size(620, 460);
            StartPosition = FormStartPosition.CenterScreen;
            Font = new Font("Segoe UI", 9F);
            try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch { }

            var title = new Label {
                Text = "Saints Row PC", Font = new Font("Segoe UI", 16F, FontStyle.Bold),
                AutoSize = true, Location = new Point(16, 12)
            };
            var intro = new Label {
                Text = "Builds the PC version of Saints Row from your own Xbox 360 disc image. " +
                       "Anything missing (Git, the Visual Studio C++ build tools, the Visual C++ runtime) " +
                       "is downloaded and installed automatically. The build takes 30-90 minutes " +
                       "and needs about 25 GB of free space (including the build tools).",
                Location = new Point(18, 48), Size = new Size(684, 48),
                Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right
            };

            var isoLabel = new Label { Text = "Saints Row disc image (.iso):", AutoSize = true, Location = new Point(18, 104) };
            isoBox = new TextBox { Location = new Point(18, 124), Width = 580, Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right };
            isoBrowse = new Button { Text = "Browse...", Location = new Point(606, 122), Width = 96, Anchor = AnchorStyles.Top | AnchorStyles.Right };
            isoBrowse.Click += delegate {
                using (var d = new OpenFileDialog { Filter = "Disc image (*.iso)|*.iso|All files (*.*)|*.*", Title = "Select your Saints Row disc image" })
                    if (d.ShowDialog(this) == DialogResult.OK) isoBox.Text = d.FileName;
            };

            var dirLabel = new Label { Text = "Install folder (a short path without spaces works best):", AutoSize = true, Location = new Point(18, 156) };
            dirBox = new TextBox { Location = new Point(18, 176), Width = 580, Text = DefaultInstallDir(), Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right };
            dirBrowse = new Button { Text = "Browse...", Location = new Point(606, 174), Width = 96, Anchor = AnchorStyles.Top | AnchorStyles.Right };
            dirBrowse.Click += delegate {
                using (var d = new FolderBrowserDialog { Description = "Choose where Saints Row PC is installed", ShowNewFolderButton = true })
                    if (d.ShowDialog(this) == DialogResult.OK) dirBox.Text = Path.Combine(d.SelectedPath, "SaintsRowPC");
            };

            desktopShortcut = new CheckBox { Text = "Create a desktop shortcut", Checked = true, AutoSize = true, Location = new Point(18, 208) };

            statusLabel = new Label { Text = "Ready.", Location = new Point(18, 238), Size = new Size(684, 20), Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right };
            progress = new ProgressBar { Location = new Point(18, 260), Size = new Size(684, 18), Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right };

            logBox = new TextBox {
                Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Vertical, WordWrap = false,
                Font = new Font("Consolas", 8.5F), BackColor = Color.FromArgb(24, 24, 24), ForeColor = Color.Gainsboro,
                Location = new Point(18, 286), Size = new Size(684, 206),
                Anchor = AnchorStyles.Top | AnchorStyles.Bottom | AnchorStyles.Left | AnchorStyles.Right
            };

            installBtn = new Button { Text = "Install", Location = new Point(502, 502), Size = new Size(96, 28), Anchor = AnchorStyles.Bottom | AnchorStyles.Right };
            cancelBtn = new Button { Text = "Close", Location = new Point(606, 502), Size = new Size(96, 28), Anchor = AnchorStyles.Bottom | AnchorStyles.Right };
            playBtn = new Button { Text = "Play", Location = new Point(398, 502), Size = new Size(96, 28), Anchor = AnchorStyles.Bottom | AnchorStyles.Right, Visible = false };
            var link = new LinkLabel { Text = "Project page", AutoSize = true, Location = new Point(18, 508), Anchor = AnchorStyles.Bottom | AnchorStyles.Left };
            link.LinkClicked += delegate { OpenUrl(Config.RepoPage); };

            installBtn.Click += delegate { StartInstall(); };
            cancelBtn.Click += delegate { Close(); };
            playBtn.Click += delegate { LaunchGame(); };
            AcceptButton = installBtn;

            Controls.AddRange(new Control[] { title, intro, isoLabel, isoBox, isoBrowse, dirLabel, dirBox, dirBrowse,
                desktopShortcut, statusLabel, progress, logBox, installBtn, cancelBtn, playBtn, link });
        }

        static string DefaultInstallDir()
        {
            string drive = Path.GetPathRoot(Environment.GetFolderPath(Environment.SpecialFolder.Windows));
            if (string.IsNullOrEmpty(drive)) drive = @"C:\";
            return Path.Combine(drive, @"Games\SaintsRowPC");
        }

        // ------------------------------------------------------------------ UI helpers

        void Ui(Action a) { if (IsDisposed) return; if (InvokeRequired) { try { BeginInvoke(a); } catch { } } else a(); }

        void Status(string text) { Log("== " + text + " =="); Ui(delegate { statusLabel.Text = text; }); }

        void Progress(int percent)
        {
            Ui(delegate {
                if (percent < 0) { progress.Style = ProgressBarStyle.Marquee; }
                else { progress.Style = ProgressBarStyle.Continuous; progress.Value = Math.Max(0, Math.Min(100, percent)); }
            });
        }

        void Log(string line)
        {
            lock (logLock) { if (logFile != null) { try { logFile.WriteLine(line); logFile.Flush(); } catch { } } }
            Ui(delegate {
                if (logBox.TextLength > 400000) logBox.Text = logBox.Text.Substring(logBox.TextLength - 200000);
                logBox.AppendText(line + Environment.NewLine);
            });
        }

        void SetRunning(bool running)
        {
            Ui(delegate {
                foreach (Control c in new Control[] { isoBox, dirBox, isoBrowse, dirBrowse, installBtn, desktopShortcut }) c.Enabled = !running;
                cancelBtn.Text = running ? "Cancel" : "Close";
            });
        }

        protected override void OnFormClosing(FormClosingEventArgs e)
        {
            if (worker != null && worker.IsAlive)
            {
                if (MessageBox.Show(this, "Stop the installation? You can run Setup again later and it continues where it stopped.",
                        Text, MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes) { e.Cancel = true; return; }
                cancelled = true;
                KillCurrent();
            }
            base.OnFormClosing(e);
        }

        // ------------------------------------------------------------------ install flow

        void StartInstall()
        {
            string iso = isoBox.Text.Trim().Trim('"');
            string dir = dirBox.Text.Trim().Trim('"');
            bool gameExtracted = File.Exists(Path.Combine(dir, @"dist\game\default.xex"));
            if (!gameExtracted && (iso.Length == 0 || !File.Exists(iso)))
            { MessageBox.Show(this, "Select your Saints Row disc image (.iso) first.", Text, MessageBoxButtons.OK, MessageBoxIcon.Warning); return; }
            if (dir.Length < 4 || !Path.IsPathRooted(dir))
            { MessageBox.Show(this, "Choose a full install folder path, for example C:\\Games\\SaintsRowPC.", Text, MessageBoxButtons.OK, MessageBoxIcon.Warning); return; }
            if (dir.Length > 60 &&
                MessageBox.Show(this, "The install folder path is long, which can make the build fail. Continue anyway?", Text,
                    MessageBoxButtons.YesNo, MessageBoxIcon.Warning) != DialogResult.Yes) return;
            if (Directory.Exists(dir) && !Directory.Exists(Path.Combine(dir, ".git")) && HasOtherFiles(dir))
            { MessageBox.Show(this, "The install folder already contains other files. Choose an empty or new folder.", Text, MessageBoxButtons.OK, MessageBoxIcon.Warning); return; }

            try
            {
                var free = new DriveInfo(Path.GetPathRoot(dir)).AvailableFreeSpace;
                if (free < Config.NeededFreeBytes && !Directory.Exists(Path.Combine(dir, "build")) &&
                    MessageBox.Show(this, string.Format("Only {0:F0} GB is free on that drive. About 25 GB is needed. Continue anyway?",
                        free / 1073741824.0), Text, MessageBoxButtons.YesNo, MessageBoxIcon.Warning) != DialogResult.Yes) return;
            }
            catch { }

            cancelled = false;
            logBox.Clear();
            playBtn.Visible = false;
            SetRunning(true);
            bool shortcut = desktopShortcut.Checked;
            worker = new Thread(delegate() { Run(iso, dir, shortcut); }) { IsBackground = true };
            worker.SetApartmentState(ApartmentState.STA);
            worker.Start();
        }

        void Run(string iso, string dir, bool shortcut)
        {
            try
            {
                Directory.CreateDirectory(dir);
                lock (logLock) logFile = new StreamWriter(Path.Combine(dir, "setup-installer.log"), true);
                Log("Saints Row PC Setup " + Config.Version + " - " + DateTime.Now);
                Log("Install folder: " + dir);

                EnsureGit();
                string vs = EnsureBuildTools();
                Log("Visual Studio: " + vs);
                EnsureVcRuntime();
                GetSource(dir);
                BuildGame(dir, iso);
                CreateShortcuts(dir, shortcut);

                Progress(100);
                Status("Done! Saints Row PC is installed.");
                Log("Run " + Path.Combine(dir, @"dist\WhompaysModLoader.exe") + " to choose mods and play.");
                Ui(delegate {
                    playBtn.Visible = true;
                    MessageBox.Show(this, "Saints Row PC is installed. Press Play, or use the Saints Row PC shortcut.", Text,
                        MessageBoxButtons.OK, MessageBoxIcon.Information);
                });
            }
            catch (Exception ex)
            {
                Progress(0);
                if (cancelled) { Status("Cancelled."); }
                else
                {
                    Status("Setup failed.");
                    Log("ERROR: " + ex.Message);
                    Ui(delegate {
                        MessageBox.Show(this, ex.Message + "\n\nThe full log is in setup-installer.log in the install folder. " +
                            "Fix the problem and press Install again; finished steps are skipped.", Text,
                            MessageBoxButtons.OK, MessageBoxIcon.Error);
                    });
                }
            }
            finally
            {
                lock (logLock) { if (logFile != null) { logFile.Dispose(); logFile = null; } }
                SetRunning(false);
            }
        }

        static bool HasOtherFiles(string dir)
        {
            foreach (var e in Directory.GetFileSystemEntries(dir))
            {
                string n = Path.GetFileName(e);
                if (!n.Equals("setup-installer.log", StringComparison.OrdinalIgnoreCase) && !n.Equals(".download", StringComparison.OrdinalIgnoreCase)) return true;
            }
            return false;
        }

        void CheckCancel() { if (cancelled) throw new StepFailed("Cancelled."); }

        // ------------------------------------------------------------------ Git

        void EnsureGit()
        {
            Status("Checking Git");
            string found = FindOnPath("git.exe");
            if (found == null)
                foreach (var root in new[] { Environment.GetEnvironmentVariable("ProgramW6432"), Environment.GetEnvironmentVariable("ProgramFiles"),
                                             Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Programs") })
                {
                    if (string.IsNullOrEmpty(root)) continue;
                    string p = Path.Combine(root, @"Git\cmd\git.exe");
                    if (File.Exists(p)) { found = p; break; }
                }
            string local = Path.Combine(ToolsDir(), "git");
            if (found == null && File.Exists(Path.Combine(local, @"cmd\git.exe"))) found = Path.Combine(local, @"cmd\git.exe");
            if (found == null)
            {
                Status("Downloading Git");
                string zip = Path.Combine(Path.GetTempPath(), "SaintsRowPC-MinGit.zip");
                Download(Config.MinGitUrl, zip);
                if (Directory.Exists(local)) Directory.Delete(local, true);
                ZipFile.ExtractToDirectory(zip, local);
                TryDelete(zip);
                found = Path.Combine(local, @"cmd\git.exe");
                if (!File.Exists(found)) throw new StepFailed("Git could not be unpacked.");
            }
            gitDir = Path.GetDirectoryName(found);
            Log("Git: " + found);
        }

        static string ToolsDir()
        {
            string d = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), @"SaintsRowPC\tools");
            Directory.CreateDirectory(d);
            return d;
        }

        // ------------------------------------------------------------------ Visual Studio Build Tools

        static string VsWhere()
        {
            string p = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86), @"Microsoft Visual Studio\Installer\vswhere.exe");
            return File.Exists(p) ? p : null;
        }

        static string QueryVs(string products, string extraArgs)
        {
            string vswhere = VsWhere();
            if (vswhere == null) return null;
            string output = RunCapture(vswhere, "-latest -products " + products + " -version \"[17.0,18.0)\" " + extraArgs + " -property installationPath");
            string first = (output ?? "").Trim().Split('\n')[0].Trim();
            return first.Length > 0 && Directory.Exists(first) ? first : null;
        }

        static string FindCompleteVs()
        {
            return QueryVs("*", "-requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 Microsoft.VisualStudio.Component.VC.Llvm.Clang");
        }

        string EnsureBuildTools()
        {
            Status("Checking the Visual Studio C++ build tools");
            string vs = FindCompleteVs();
            if (vs != null) return vs;

            string bootstrapper = Path.Combine(Path.GetTempPath(), "SaintsRowPC-vs_BuildTools.exe");
            Status("Downloading the Visual Studio C++ build tools");
            Download(Config.BuildToolsUrl, bootstrapper);

            string adds = "";
            foreach (var c in Config.VsComponents) adds += " --add " + c;
            // Add the missing parts to an existing Build Tools install, otherwise install Build Tools.
            string existing = QueryVs("Microsoft.VisualStudio.Product.BuildTools", "");
            string args = existing != null
                ? "modify --installPath \"" + existing + "\"" + adds + " --includeRecommended --passive --wait --norestart"
                : adds.Trim() + " --includeRecommended --passive --wait --norestart";

            Status("Installing the Visual Studio C++ build tools (this can take 10-30 minutes)");
            Log("A Visual Studio Installer window shows the progress. Windows asks for permission first.");
            Progress(-1);
            int code = RunElevated(bootstrapper, args);
            TryDelete(bootstrapper);
            Log("Visual Studio Installer exit code: " + code);
            CheckCancel();
            if (code == 1602 || code == 1223) throw new StepFailed("The Visual Studio build tools installation was cancelled.");
            if (code == 3010) Log("Windows needs a restart to finish the build tools setup. Setup continues; restart if the build fails.");
            else if (code != 0) throw new StepFailed("Installing the Visual Studio build tools failed (exit code " + code + ").");

            vs = FindCompleteVs();
            if (vs == null)
                throw new StepFailed("The Visual Studio build tools were installed but the C++ or Clang components are missing. " +
                    "Open the Visual Studio Installer, modify Build Tools 2022 and tick 'Desktop development with C++' and " +
                    "'C++ Clang Compiler for Windows'.");
            return vs;
        }

        // ------------------------------------------------------------------ Visual C++ runtime

        void EnsureVcRuntime()
        {
            Status("Checking the Visual C++ runtime");
            if (VcRuntimeInstalled()) { Log("Visual C++ runtime: installed"); return; }
            string exe = Path.Combine(Path.GetTempPath(), "SaintsRowPC-vc_redist.x64.exe");
            Status("Downloading the Visual C++ runtime");
            Download(Config.VcRedistUrl, exe);
            Status("Installing the Visual C++ runtime");
            Progress(-1);
            int code = RunElevated(exe, "/install /passive /norestart");
            TryDelete(exe);
            Log("Visual C++ runtime installer exit code: " + code);
            CheckCancel();
            if (code != 0 && code != 3010 && code != 1638 && code != 1641)
                throw new StepFailed("Installing the Visual C++ runtime failed (exit code " + code + ").");
        }

        static bool VcRuntimeInstalled()
        {
            try
            {
                using (var hk = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry64))
                using (var k = hk.OpenSubKey(@"SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64"))
                {
                    if (k == null) return false;
                    object installed = k.GetValue("Installed"), major = k.GetValue("Major"), minor = k.GetValue("Minor");
                    return installed is int && (int)installed == 1 && major is int && (int)major >= 14 && minor is int && (int)minor >= 40;
                }
            }
            catch { return false; }
        }

        // ------------------------------------------------------------------ Source code

        void GetSource(string dir)
        {
            string git = Path.Combine(gitDir, "git.exe");
            if (Directory.Exists(Path.Combine(dir, ".git")))
            {
                Status("Updating the Saints Row PC source code");
                Progress(-1);
                int c = RunLogged(git, "-C \"" + dir + "\" pull --ff-only", dir, null);
                if (c != 0) Log("Could not update the source code (exit code " + c + "); using the copy already downloaded.");
                return;
            }
            Status("Downloading the Saints Row PC source code");
            Progress(-1);
            string tmp = Path.Combine(dir, ".download");
            if (Directory.Exists(tmp)) ForceDelete(tmp);
            int code = RunLogged(git, "clone --depth 1 --branch " + Config.Branch + " --progress " + Config.RepoUrl + " \"" + tmp + "\"", dir, null);
            CheckCancel();
            if (code != 0) throw new StepFailed("Downloading the source code failed (git exit code " + code + "). Check your internet connection.");
            foreach (var entry in Directory.GetFileSystemEntries(tmp))
            {
                string target = Path.Combine(dir, Path.GetFileName(entry));
                if (Directory.Exists(entry)) Directory.Move(entry, target); else File.Move(entry, target);
            }
            ForceDelete(tmp);
        }

        // ------------------------------------------------------------------ Build

        static readonly Regex NinjaProgress = new Regex(@"^\[(\d+)/(\d+)\]");

        void BuildGame(string dir, string iso)
        {
            Status("Building Saints Row PC (30-90 minutes)");
            Progress(-1);
            string script = Path.Combine(dir, @"scripts\setup.ps1");
            if (!File.Exists(script)) throw new StepFailed("scripts\\setup.ps1 is missing from the source code download.");
            string ps = Path.Combine(Environment.SystemDirectory, @"WindowsPowerShell\v1.0\powershell.exe");
            string args = "-NoProfile -ExecutionPolicy Bypass -File \"" + script + "\" -NoPause";
            if (iso.Length > 0 && File.Exists(iso)) args += " -Iso \"" + iso + "\"";

            string stage = "";
            int code = RunLogged(ps, args, dir, delegate(string line) {
                string t = line.Trim();
                if (t.StartsWith("== ") && t.EndsWith(" ==")) { stage = t.Trim('=', ' '); Ui(delegate { statusLabel.Text = stage; }); Progress(-1); }
                var m = NinjaProgress.Match(t);
                if (m.Success)
                {
                    int done = int.Parse(m.Groups[1].Value), total = int.Parse(m.Groups[2].Value);
                    if (total > 0) { Progress(done * 100 / total); Ui(delegate { statusLabel.Text = stage + string.Format(" ({0}/{1})", done, total); }); }
                }
            });
            CheckCancel();
            if (code != 0) throw new StepFailed("The build failed (exit code " + code + "). See the log above and build\\setup.log in the install folder.");
            if (!File.Exists(Path.Combine(dir, @"dist\saintsrow.exe"))) throw new StepFailed("The build finished but dist\\saintsrow.exe is missing.");
        }

        // ------------------------------------------------------------------ Shortcuts

        void CreateShortcuts(string dir, bool desktop)
        {
            Status("Creating shortcuts");
            string dist = Path.Combine(dir, "dist");
            string target = Path.Combine(dist, "WhompaysModLoader.exe");
            if (!File.Exists(target)) target = Path.Combine(dist, "saintsrow.exe");
            try
            {
                string menu = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Programs), "Saints Row PC");
                Directory.CreateDirectory(menu);
                MakeShortcut(Path.Combine(menu, "Saints Row PC.lnk"), target, dist, "Play Saints Row PC");
                MakeShortcut(Path.Combine(menu, "Saints Row PC (no mod menu).lnk"), Path.Combine(dist, "saintsrow.exe"), dist, "Play Saints Row PC without the mod loader");
                string setupCopy = Path.Combine(dir, "SaintsRowPC-Setup.exe");
                try { if (!string.Equals(Application.ExecutablePath, setupCopy, StringComparison.OrdinalIgnoreCase)) File.Copy(Application.ExecutablePath, setupCopy, true); } catch { }
                if (File.Exists(setupCopy)) MakeShortcut(Path.Combine(menu, "Update or repair Saints Row PC.lnk"), setupCopy, dir, "Update or rebuild Saints Row PC");
                if (desktop)
                    MakeShortcut(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory), "Saints Row PC.lnk"), target, dist, "Play Saints Row PC");
            }
            catch (Exception ex) { Log("Could not create shortcuts: " + ex.Message); }
        }

        static void MakeShortcut(string lnk, string target, string workDir, string description)
        {
            Type t = Type.GetTypeFromProgID("WScript.Shell");
            object shell = Activator.CreateInstance(t);
            object sc = t.InvokeMember("CreateShortcut", BindingFlags.InvokeMethod, null, shell, new object[] { lnk });
            Type st = sc.GetType();
            st.InvokeMember("TargetPath", BindingFlags.SetProperty, null, sc, new object[] { target });
            st.InvokeMember("WorkingDirectory", BindingFlags.SetProperty, null, sc, new object[] { workDir });
            st.InvokeMember("Description", BindingFlags.SetProperty, null, sc, new object[] { description });
            st.InvokeMember("IconLocation", BindingFlags.SetProperty, null, sc, new object[] { target + ",0" });
            st.InvokeMember("Save", BindingFlags.InvokeMethod, null, sc, null);
        }

        void LaunchGame()
        {
            string dist = Path.Combine(dirBox.Text.Trim().Trim('"'), "dist");
            string exe = Path.Combine(dist, "WhompaysModLoader.exe");
            if (!File.Exists(exe)) exe = Path.Combine(dist, "saintsrow.exe");
            try { Process.Start(new ProcessStartInfo(exe) { WorkingDirectory = dist, UseShellExecute = true }); Close(); }
            catch (Exception ex) { MessageBox.Show(this, ex.Message, Text, MessageBoxButtons.OK, MessageBoxIcon.Error); }
        }

        // ------------------------------------------------------------------ process / download helpers

        void Download(string url, string file)
        {
            Log("Downloading " + url);
            for (int attempt = 1; ; attempt++)
            {
                try
                {
                    var req = (HttpWebRequest)WebRequest.Create(url);
                    req.UserAgent = "SaintsRowPC-Setup/" + Config.Version;
                    req.AllowAutoRedirect = true;
                    req.Timeout = 60000;
                    req.ReadWriteTimeout = 60000;
                    using (var resp = (HttpWebResponse)req.GetResponse())
                    using (var input = resp.GetResponseStream())
                    using (var output = File.Create(file))
                    {
                        long total = resp.ContentLength, done = 0;
                        var buf = new byte[1 << 16];
                        int n, last = -1;
                        Progress(total > 0 ? 0 : -1);
                        while ((n = input.Read(buf, 0, buf.Length)) > 0)
                        {
                            CheckCancel();
                            output.Write(buf, 0, n);
                            done += n;
                            if (total > 0) { int pct = (int)(done * 100 / total); if (pct != last) { last = pct; Progress(pct); } }
                        }
                        if (total > 0 && done != total) throw new IOException("Download was cut short.");
                    }
                    return;
                }
                catch (StepFailed) { throw; }
                catch (Exception ex)
                {
                    TryDelete(file);
                    if (attempt >= 3) throw new StepFailed("Download failed: " + url + "\n" + ex.Message);
                    Log("Download failed (" + ex.Message + "), retrying...");
                    Thread.Sleep(3000);
                }
            }
        }

        int RunElevated(string exe, string args)
        {
            Log("> " + Path.GetFileName(exe) + " " + args);
            var psi = new ProcessStartInfo(exe, args) { UseShellExecute = true, Verb = "runas" };
            try
            {
                using (var p = Process.Start(psi))
                {
                    current = p;
                    p.WaitForExit();
                    current = null;
                    return p.ExitCode;
                }
            }
            catch (System.ComponentModel.Win32Exception ex)
            {
                if (ex.NativeErrorCode == 1223) throw new StepFailed("Administrator permission is needed to install the build tools.");
                throw;
            }
        }

        int RunLogged(string exe, string args, string workDir, Action<string> onLine)
        {
            Log("> " + Path.GetFileName(exe) + " " + args);
            var psi = new ProcessStartInfo(exe, args) {
                UseShellExecute = false, CreateNoWindow = true, WorkingDirectory = workDir,
                RedirectStandardOutput = true, RedirectStandardError = true,
            };
            if (gitDir != null) psi.EnvironmentVariables["PATH"] = gitDir + ";" + psi.EnvironmentVariables["PATH"];
            psi.EnvironmentVariables["GIT_TERMINAL_PROMPT"] = "0";
            using (var p = new Process { StartInfo = psi })
            {
                DataReceivedEventHandler h = delegate(object s, DataReceivedEventArgs e) {
                    if (e.Data == null) return;
                    Log(e.Data);
                    if (onLine != null) { try { onLine(e.Data); } catch { } }
                };
                p.OutputDataReceived += h;
                p.ErrorDataReceived += h;
                p.Start();
                current = p;
                p.BeginOutputReadLine();
                p.BeginErrorReadLine();
                p.WaitForExit();
                current = null;
                return p.ExitCode;
            }
        }

        void KillCurrent()
        {
            var p = current;
            if (p == null) return;
            try { Process.Start(new ProcessStartInfo("taskkill", "/T /F /PID " + p.Id) { CreateNoWindow = true, UseShellExecute = false }).WaitForExit(10000); } catch { }
        }

        static string RunCapture(string exe, string args)
        {
            try
            {
                var psi = new ProcessStartInfo(exe, args) { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true };
                using (var p = Process.Start(psi)) { string o = p.StandardOutput.ReadToEnd(); p.WaitForExit(); return o; }
            }
            catch { return null; }
        }

        static string FindOnPath(string file)
        {
            foreach (var d in (Environment.GetEnvironmentVariable("PATH") ?? "").Split(';'))
            {
                try { string p = Path.Combine(d.Trim().Trim('"'), file); if (d.Trim().Length > 0 && File.Exists(p)) return p; } catch { }
            }
            return null;
        }

        static void TryDelete(string f) { try { if (File.Exists(f)) File.Delete(f); } catch { } }

        static void ForceDelete(string dir)
        {
            foreach (var f in Directory.GetFiles(dir, "*", SearchOption.AllDirectories)) { try { File.SetAttributes(f, FileAttributes.Normal); } catch { } }
            Directory.Delete(dir, true);
        }

        static void OpenUrl(string url) { try { Process.Start(new ProcessStartInfo(url) { UseShellExecute = true }); } catch { } }
    }
}
