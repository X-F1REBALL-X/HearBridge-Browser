//go:build windows

// HearBridge Browser for Windows.
// A native window hosting Edge WebView2 that loads the HearBridge Browser
// player page served by the PS5 payload at http://<console-ip>:8096/.
package main

import (
	"encoding/json"
	"html"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"time"

	webview2 "github.com/jchv/go-webview2"
)

const (
	appName     = "HearBridge Browser"
	appVersion  = "1.0.0"
	defaultIP   = "192.168.1.50"
	defaultPort = "8096"
)

type settings struct {
	ConsoleIP string `json:"consoleIp"`
}

func appDataDir() string {
	base := os.Getenv("APPDATA")
	if base == "" {
		base, _ = os.UserConfigDir()
	}
	return filepath.Join(base, appName)
}

func settingsPath() string { return filepath.Join(appDataDir(), "settings.json") }

func loadSettings() settings {
	s := settings{ConsoleIP: defaultIP}
	if b, err := os.ReadFile(settingsPath()); err == nil {
		var t settings
		if json.Unmarshal(b, &t) == nil && strings.TrimSpace(t.ConsoleIP) != "" {
			s.ConsoleIP = strings.TrimSpace(t.ConsoleIP)
		}
	}
	return s
}

func saveSettings(s settings) {
	_ = os.MkdirAll(appDataDir(), 0o755)
	b, _ := json.MarshalIndent(s, "", "  ")
	_ = os.WriteFile(settingsPath(), b, 0o644)
}

// normalize turns "192.168.1.50", "http://192.168.1.50:8096/" etc. into
// "host:port" (port defaults to 8096).
func normalize(in string) string {
	s := strings.TrimSpace(in)
	s = strings.TrimPrefix(s, "http://")
	s = strings.TrimPrefix(s, "https://")
	if i := strings.IndexAny(s, "/?#"); i >= 0 {
		s = s[:i]
	}
	if s == "" {
		return ""
	}
	if _, _, err := net.SplitHostPort(s); err != nil {
		s = net.JoinHostPort(strings.Trim(s, "[]"), defaultPort)
	}
	return s
}

// probe returns nil if the HearBridge Browser server answers at hostport.
func probe(hostport string) error {
	c := &http.Client{Timeout: 4 * time.Second}
	resp, err := c.Get("http://" + hostport + "/status")
	if err != nil {
		return err
	}
	resp.Body.Close()
	return nil
}

const pageStyle = `<style>
html,body{margin:0;height:100%;background:#07101f;color:#e6f0ff;font-family:"Segoe UI",system-ui,sans-serif}
.wrap{min-height:100%;display:flex;align-items:center;justify-content:center}
.card{background:#0d1a33;border:1px solid #2f6fe0;border-radius:14px;padding:28px 32px;max-width:440px;width:90%;box-shadow:0 0 30px #1f5fd055}
h1{margin:0 0 6px;font-size:22px} .sub{color:#6aa8ff;letter-spacing:.25em;font-size:12px;margin-bottom:18px}
p{color:#b9c8e6;line-height:1.45} .err{color:#ff8f8f;font-size:13px;word-break:break-word}
label{display:block;font-size:13px;color:#b9c8e6;margin:14px 0 6px}
.row{display:flex;gap:8px}
input{flex:1;padding:10px 12px;border-radius:8px;border:1px solid #2f6fe0;background:#07101f;color:#fff;font-size:15px}
button{padding:10px 18px;border-radius:8px;border:0;background:#2f7bff;color:#fff;font-size:15px;cursor:pointer}
button:disabled{opacity:.6;cursor:default}
.spin{display:inline-block;width:14px;height:14px;border:2px solid #6aa8ff;border-top-color:transparent;border-radius:50%;animation:s 1s linear infinite;vertical-align:-2px;margin-right:8px}
@keyframes s{to{transform:rotate(360deg)}}
</style>`

func connectingPage(hostport string) string {
	return `<!doctype html><html><head><meta charset="utf-8">` + pageStyle + `</head><body><div class="wrap"><div class="card">
<h1>HearBridge</h1><div class="sub">BROWSER</div>
<p><span class="spin"></span>Connecting to ` + html.EscapeString(hostport) + ` ...</p></div></div></body></html>`
}

func errorPage(ip string, hostport string, errMsg string) string {
	return `<!doctype html><html><head><meta charset="utf-8">` + pageStyle + `</head><body><div class="wrap"><div class="card">
<h1>HearBridge</h1><div class="sub">BROWSER</div>
<p>Can't reach the HearBridge Browser payload at <b>http://` + html.EscapeString(hostport) + `/</b>.</p>
<p>Make sure the PS5 is on, the payload is running and this PC is on the same network.</p>
<div class="err">` + html.EscapeString(errMsg) + `</div>
<label for="ip">Console IP</label>
<div class="row"><input id="ip" value="` + html.EscapeString(ip) + `" spellcheck="false" autofocus>
<button id="retry">Retry</button></div></div></div>
<script>
const b=document.getElementById('retry'),i=document.getElementById('ip');
function go(){b.disabled=true;b.textContent='Connecting...';window.hbConnect(i.value);}
b.onclick=go;i.addEventListener('keydown',e=>{if(e.key==='Enter')go();});
i.focus();i.select();
</script></body></html>`
}

func main() {
	// Allow the player to start audio without a click.
	args := "--autoplay-policy=no-user-gesture-required"
	if extra := os.Getenv("WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS"); extra != "" {
		args = extra + " " + args
	}
	os.Setenv("WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS", args)

	local := os.Getenv("LOCALAPPDATA")
	if local == "" {
		local = appDataDir()
	}
	dataPath := filepath.Join(local, appName, "WebView2")

	w := webview2.NewWithOptions(webview2.WebViewOptions{
		Debug:     false,
		AutoFocus: true,
		DataPath:  dataPath,
		WindowOptions: webview2.WindowOptions{
			Title:  appName + " " + appVersion,
			Width:  1100,
			Height: 760,
			IconId: 1, // RT_GROUP_ICON #1 (winres/winres.json)
			Center: true,
		},
	})
	if w == nil {
		msgBox(appName,
			"Could not start Microsoft Edge WebView2.\n\n"+
				"Install the WebView2 Runtime from\nhttps://developer.microsoft.com/microsoft-edge/webview2/\nand try again.")
		os.Exit(1)
	}
	defer w.Destroy()
	w.SetSize(640, 480, webview2.HintMin)

	st := loadSettings()

	var connect func(ip string)
	connect = func(ip string) {
		hp := normalize(ip)
		if hp == "" {
			w.SetHtml(errorPage(ip, ip, "Please enter the console IP address."))
			return
		}
		w.SetHtml(connectingPage(hp))
		go func() {
			err := probe(hp)
			w.Dispatch(func() {
				if err != nil {
					w.SetHtml(errorPage(strings.TrimSpace(ip), hp, err.Error()))
					return
				}
				st.ConsoleIP = strings.TrimSpace(ip)
				saveSettings(st)
				w.Navigate("http://" + hp + "/")
			})
		}()
	}

	// Called from the error page's Retry button. Returns at once; the probe
	// runs in the background so the window stays responsive.
	_ = w.Bind("hbConnect", func(ip string) {
		w.Dispatch(func() { connect(ip) })
	})

	connect(st.ConsoleIP)
	w.Run()
}

func msgBox(title, text string) {
	t, _ := syscallUTF16(title)
	m, _ := syscallUTF16(text)
	procMessageBoxW.Call(0, uintptr(m), uintptr(t), 0x10) // MB_ICONERROR
}

