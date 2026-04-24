// Shared ON8CIT visual identity: the CSS and SVG logo used by every web
// page served by the radio (main UI in web.cpp, provisioning portal in
// provision.cpp). Kept in one translation unit so the two pages can't
// drift. Palette tracks the OLED theme (yellow + cyan wordmark on deep
// black; orange accent; 2.4 GHz broadcast-waves logo).

#include <Arduino.h>

const char *on8citPageCss() {
    return
        "*{box-sizing:border-box}"
        "body{font-family:-apple-system,Segoe UI,sans-serif;max-width:720px;"
        "margin:0 auto;padding:0 1em 2em;color:#e6e7ea;background:#0b0d11}"
        "header{display:flex;align-items:center;gap:.7em;margin:0 -1em 1em;"
        "padding:1em 1.1em;background:linear-gradient(180deg,#15171c,#0f1116);"
        "border-bottom:1px solid #222}"
        "header .logo{flex:0 0 40px;width:40px;height:40px}"
        "header h1{margin:0;font-size:1.25em;font-weight:700;letter-spacing:.06em}"
        "header h1 .yel{color:#FFD400}"
        "header h1 .cya{color:#5ae3ff}"
        ".card{background:#141720;border:1px solid #242832;border-radius:8px;"
        "padding:.9em 1.1em;margin:.6em 0}"
        ".card h2{margin:0 0 .5em;font-size:.95em;color:#FFD400;"
        "letter-spacing:.08em;text-transform:uppercase}"
        ".kv{display:grid;grid-template-columns:8em 1fr;gap:.3em 1em;"
        "font-family:ui-monospace,Menlo,monospace;font-size:.9em;color:#cdd3de}"
        ".kv div:nth-child(odd){color:#8aa}"
        ".grid{display:grid;grid-template-columns:repeat(auto-fill,"
        "minmax(180px,1fr));gap:.4em}"
        ".stationRow{display:flex;gap:.3em;align-items:stretch}"
        ".stationPick{flex:1}"
        ".station{padding:.55em .7em;border:1px solid #2a2f3c;border-radius:6px;"
        "background:#181b24;color:#e6e7ea;cursor:pointer;display:block;width:100%;"
        "text-align:left;font:inherit}"
        ".station.cur{border-color:#FFD400;background:#1f2330;"
        "box-shadow:0 0 0 1px #FFD40066}"
        ".station small{display:block;color:#8aa;margin-top:.2em;"
        "word-break:break-all}"
        ".infoBtn{background:#181b24;color:#5ae3ff;border:1px solid #2a2f3c;"
        "border-radius:6px;padding:.3em .6em;cursor:pointer;font-size:1.15em;"
        "width:auto}"
        ".infoBtn:hover{background:#20242f}"
        "input,button,select{width:100%;padding:.55em;font-size:1em;"
        "margin:.25em 0;border-radius:5px;border:1px solid #2a2f3c;"
        "background:#0f1218;color:#e6e7ea;font:inherit}"
        "input::placeholder{color:#556}"
        "button{background:#FFD400;color:#111;border:0;padding:.75em;"
        "font-weight:600;cursor:pointer}"
        "button:hover{background:#FFE43c}"
        "button.warn{background:#e24;color:#fff}"
        "button.warn:hover{background:#f35}"
        "label{display:block;font-size:.85em;color:#8aa;margin-top:.5em}"
        ".row{display:flex;gap:.5em;align-items:center;flex-wrap:wrap}"
        ".row>form,.row>button{flex:0 0 auto;width:auto}"
        ".row>form button{width:auto;padding:.6em 1em}"
        "#modalBg{display:none;position:fixed;inset:0;background:rgba(0,0,0,.6);"
        "backdrop-filter:blur(3px)}"
        "#modalBg.show,#modal.show{display:block}"
        "#modal{display:none;position:fixed;top:50%;left:50%;"
        "transform:translate(-50%,-50%);background:#141720;"
        "border:1px solid #2a2f3c;padding:1em 1.2em;border-radius:8px;"
        "box-shadow:0 8px 32px rgba(0,0,0,.5);max-width:94%;width:480px;z-index:10}"
        "#modal h2{margin:.2em 0 .6em;color:#FFD400;letter-spacing:.06em}"
        ".net{padding:.5em .6em;margin:.15em 0;border:1px solid #242832;"
        "border-radius:6px;background:#181b24;cursor:pointer}"
        ".net:hover{background:#20242f;border-color:#5ae3ff}"
        ".net .rssi{color:#8aa;font-size:.85em;float:right;margin-left:.5em}"
        "a{color:#5ae3ff}"
        "footer{color:#556;text-align:center;margin:1em 0;font-size:.8em}"
        ".bigDiscover{display:block;background:linear-gradient(135deg,#1a2a44,#223b60);"
        "color:#5ae3ff;text-align:center;padding:1em;margin-bottom:.6em;border-radius:8px;"
        "border:1px solid #5ae3ff55;text-decoration:none;font-weight:600;font-size:1.05em}"
        ".bigDiscover:hover{background:linear-gradient(135deg,#223b60,#2e4f7d);"
        "border-color:#5ae3ff}";
}

const char *on8citLogoSvg() {
    return
        "<svg class=logo viewBox='0 0 40 40' xmlns='http://www.w3.org/2000/svg'>"
        "<circle cx=20 cy=20 r=16 fill=none stroke='#FFD40030' stroke-width=1/>"
        "<circle cx=20 cy=20 r=11 fill=none stroke='#FFD40080' stroke-width=1/>"
        "<circle cx=20 cy=20 r=7  fill=none stroke='#FFD400'   stroke-width=1.6/>"
        "<circle cx=20 cy=20 r=3  fill='#FFD400'/>"
        "</svg>";
}

// Standalone SVG served as /favicon.svg; includes the proper <?xml?> +
// namespace that browsers want on a top-level SVG document, and a dark
// background so the icon is visible on light tab backgrounds. Same
// broadcast-waves shape as on8citLogoSvg().
const char *on8citFaviconSvg() {
    return
        "<?xml version='1.0' encoding='UTF-8'?>"
        "<svg viewBox='0 0 40 40' xmlns='http://www.w3.org/2000/svg'>"
        "<rect width=40 height=40 rx=8 fill='#0b0d11'/>"
        "<circle cx=20 cy=20 r=16 fill=none stroke='#FFD40030' stroke-width=1/>"
        "<circle cx=20 cy=20 r=11 fill=none stroke='#FFD40080' stroke-width=1/>"
        "<circle cx=20 cy=20 r=7  fill=none stroke='#FFD400'   stroke-width=2/>"
        "<circle cx=20 cy=20 r=3  fill='#FFD400'/>"
        "</svg>";
}
