/*
 * html_pages.h
 * ─────────────────────────────────────────────────────────────────
 * Bug fixes:
 *   1. Complete visual redesign with Inter font
 *   2. Transcript tab shows FINAL transcript after meeting ends
 *      (uses new finalTranscriptText global via /api/status)
 *   3. Summary: markdown-like rendering (headers, bullets, bold)
 *   4. History: "Ask AI" button per card opens chat with that
 *      meeting's summary as context override
 *   5. Delete: query-param based, robust error handling
 *   6. Chat: shows banner when in history-meeting context mode
 *   7. Summary validated server-side; empty summaries suppressed
 * ─────────────────────────────────────────────────────────────────
 */
#pragma once
#include <Arduino.h>

const char DASHBOARD_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="transparent">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
<title>MeetingRecorder</title>
<link rel="preconnect" href="https://fonts.googleapis.com">
<link href="https://fonts.googleapis.com/css2?family=Inter:wght@300;400;500;600;700&family=JetBrains+Mono:wght@400;500&display=swap" rel="stylesheet">
<style>
*,*::before,*::after{box-sizing:border-box;margin:0;padding:0}
:root{
  --bg0:#0a0a0f;--bg1:#111118;--bg2:#18181f;--bg3:#1f1f28;--bg4:#27272f;--bg5:#31313a;
  --border:rgba(255,255,255,0.06);--border2:rgba(255,255,255,0.12);--border3:rgba(255,255,255,0.18);
  --t0:#eeeef5;--t1:#9898aa;--t2:#55556a;
  --accent:#7c5cfc;--accent2:#a485ff;--agl:rgba(124,92,252,0.14);--agl2:rgba(124,92,252,0.08);
  --red:#e05555;--green:#3ecf7a;--amber:#f5a623;--blue:#4a9eff;--teal:#2dd4bf;
  --f:'Inter',-apple-system,sans-serif;--fm:'JetBrains Mono',monospace;
  --r:10px;--rsm:6px;--rlg:14px;
  --sh:0 4px 24px rgba(0,0,0,0.5);--tr:0.16s ease
}
html{height:-webkit-fill-available}
body{height:100%;height:100dvh;overflow:hidden;-webkit-overflow-scrolling:touch;font-family:var(--f);background:var(--bg0);color:var(--t0);font-size:14px;line-height:1.6}

/* ── Layout ───────────────────────────── */
.app{display:flex;height:100vh;height:100dvh;min-height:-webkit-fill-available}
.sb{width:220px;min-width:220px;background:var(--bg1);border-right:1px solid var(--border);display:flex;flex-direction:column}
.main{flex:1;display:flex;flex-direction:column;overflow:hidden;min-width:0}

/* ── Sidebar ──────────────────────────── */
.sb-head{padding:16px 14px 13px;border-bottom:1px solid var(--border)}
.logo{display:flex;align-items:center;gap:9px;font-weight:700;font-size:14.5px;letter-spacing:-.03em;color:var(--t0);text-decoration:none}
.logo-icon{width:28px;height:28px;background:linear-gradient(135deg,var(--accent),var(--accent2));border-radius:7px;display:flex;align-items:center;justify-content:center;font-size:14px;flex-shrink:0}
.spill{display:inline-flex;align-items:center;gap:5px;margin-top:9px;padding:3px 9px;border-radius:20px;font-size:10px;font-weight:600;letter-spacing:.04em;text-transform:uppercase}
.spill.idle{background:rgba(85,85,106,0.18);color:var(--t2)}
.spill.recording{background:rgba(224,85,85,0.13);color:var(--red)}
.spill.processing{background:rgba(74,158,255,0.13);color:var(--blue)}
.spill.done{background:rgba(62,207,122,0.13);color:var(--green)}
.sdot{width:5px;height:5px;border-radius:50%;background:currentColor;flex-shrink:0}
.spill.recording .sdot{animation:pulse 1.1s ease infinite}
@keyframes pulse{0%,100%{opacity:1;transform:scale(1)}50%{opacity:.3;transform:scale(.6)}}

.sb-nav{flex:1;overflow-y:auto;padding:8px 8px 4px;display:flex;flex-direction:column;gap:1px}
.ni{display:flex;align-items:center;gap:9px;padding:8px 10px;border-radius:var(--rsm);font-size:13px;font-weight:400;color:var(--t1);cursor:pointer;background:none;border:none;width:100%;text-align:left;transition:all var(--tr)}
.ni:hover{background:var(--bg3);color:var(--t0)}
.ni.active{background:var(--agl);color:var(--accent2);font-weight:500}
.ni .ico{font-size:14px;width:19px;text-align:center;flex-shrink:0}
.ni .lbl{flex:1;font-size:13px}
.nbadge{background:var(--accent);color:#fff;font-size:9.5px;font-weight:700;padding:1px 5px;border-radius:10px;min-width:17px;text-align:center;letter-spacing:0}
.ni.locked{opacity:.35;cursor:not-allowed}
.ni.locked:hover{background:none;color:var(--t1)}
.sb-foot{padding:8px 10px 14px;border-top:1px solid var(--border)}
.sb-meta{font-size:11px;color:var(--t2);font-family:var(--fm);line-height:1.9;padding:0 2px}

/* ── Topbar ───────────────────────────── */
.topbar{height:48px;border-bottom:1px solid var(--border);display:flex;align-items:center;padding:0 20px;gap:10px;background:var(--bg1);flex-shrink:0}
.tb-title{font-size:13.5px;font-weight:600;letter-spacing:-.01em}
.tb-right{margin-left:auto;display:flex;align-items:center;gap:10px}
.tb-chip{font-size:11px;font-family:var(--fm);color:var(--t2)}
.tb-brand{font-size:11px;font-weight:600;letter-spacing:.02em;color:var(--accent2);opacity:.75;padding:2px 9px;border:1px solid rgba(164,133,255,0.2);border-radius:20px;white-space:nowrap;text-decoration:none;transition:opacity var(--tr),border-color var(--tr)}
.tb-brand:hover{opacity:1;border-color:rgba(164,133,255,0.45)}
.wvf{display:flex;align-items:center;gap:2.5px;height:18px}
.wb{width:2.5px;background:var(--red);border-radius:2px;animation:wave .65s ease-in-out infinite alternate}
.wb:nth-child(1){animation-delay:0s}.wb:nth-child(2){animation-delay:.09s}
.wb:nth-child(3){animation-delay:.18s}.wb:nth-child(4){animation-delay:.12s}
.wb:nth-child(5){animation-delay:.04s}
@keyframes wave{from{height:2px;opacity:.35}to{height:16px;opacity:1}}

/* ── Content area ─────────────────────── */
.content{flex:1;overflow-y:auto;padding:20px 22px}
.tab{display:none}.tab.active{display:block}
.chat-wrap{display:none;flex-direction:column;flex:1;overflow:hidden}.chat-wrap.active{display:flex}

/* ── Record button ────────────────────── */
.rec-btn{width:100%;padding:12px 16px;border-radius:var(--r);border:none;font-family:var(--f);font-size:14px;font-weight:600;cursor:pointer;display:flex;align-items:center;justify-content:center;gap:8px;margin-bottom:16px;transition:all var(--tr)}
.rec-btn.start{background:linear-gradient(135deg,#2ecc8a,#20a06c);color:#fff;box-shadow:0 4px 18px rgba(46,204,138,0.28)}
.rec-btn.start:hover{transform:translateY(-1px);box-shadow:0 6px 24px rgba(46,204,138,0.42)}
.rec-btn.stop{background:linear-gradient(135deg,var(--red),#b83030);color:#fff;box-shadow:0 4px 18px rgba(224,85,85,0.28);animation:rp 2s ease infinite}
.rec-btn.stop:hover{transform:translateY(-1px)}
@keyframes rp{0%,100%{box-shadow:0 4px 18px rgba(224,85,85,0.28)}50%{box-shadow:0 4px 26px rgba(224,85,85,0.55)}}
.rec-btn:disabled{opacity:.45;cursor:not-allowed;transform:none!important}

/* ── Stats grid ───────────────────────── */
.stats{display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:10px;margin-bottom:16px}
.sc{background:var(--bg2);border:1px solid var(--border);border-radius:var(--r);padding:14px 15px;transition:border-color var(--tr)}
.sc:hover{border-color:var(--border2)}
.sl{font-size:10px;text-transform:uppercase;letter-spacing:.06em;color:var(--t2);font-weight:600}
.sv{font-size:22px;font-weight:700;margin-top:3px;letter-spacing:-.03em;font-family:var(--fm)}
.su{font-size:11px;color:var(--t1);margin-top:1px}

/* ── Cards ────────────────────────────── */
.card{background:var(--bg2);border:1px solid var(--border);border-radius:var(--r);margin-bottom:12px}
.ch{padding:11px 15px;border-bottom:1px solid var(--border);display:flex;align-items:center;gap:8px}
.ct{font-size:12.5px;font-weight:600;flex:1;letter-spacing:-.01em}
.cb{padding:14px 15px}
.lbadge{display:inline-flex;align-items:center;gap:4px;padding:2px 8px;border-radius:10px;font-size:10px;font-weight:700;text-transform:uppercase;letter-spacing:.04em;background:rgba(224,85,85,0.12);color:var(--red)}
.ldot{width:5px;height:5px;border-radius:50%;background:var(--red);animation:pulse 1s ease infinite}

/* ── Typography ───────────────────────── */
.prose{font-size:13.5px;line-height:1.72;color:var(--t1);white-space:pre-wrap;word-break:break-word}
.empty-state{text-align:center;padding:28px 16px;color:var(--t2)}
.empty-state .ei{font-size:28px;margin-bottom:9px}
.empty-state p{font-size:13px}

/* ── Transcript ───────────────────────── */
.tx-banner{display:none;align-items:center;gap:8px;padding:8px 12px;margin-bottom:10px;border-radius:var(--rsm);font-size:12px;font-weight:500}
.tx-banner.live{background:rgba(224,85,85,0.08);border:1px solid rgba(224,85,85,0.18);color:var(--red)}
.tx-banner.final{background:rgba(62,207,122,0.08);border:1px solid rgba(62,207,122,0.18);color:var(--green)}
.tx-banner.trunc{background:rgba(245,166,35,0.08);border:1px solid rgba(245,166,35,0.18);color:var(--amber)}
.tx-box{font-family:var(--fm);font-size:12.5px;line-height:1.9;color:var(--t1);white-space:pre-wrap;word-break:break-word;background:var(--bg2);border:1px solid var(--border);border-radius:var(--r);padding:16px;max-height:calc(100dvh - 200px);overflow-y:auto}
.tx-empty{color:var(--t2);font-style:italic}

/* ── Summary markdown rendering ───────── */
.sum-rendered{font-size:13.5px;line-height:1.78;color:var(--t1)}
.sum-rendered h1,.sum-rendered h2,.sum-rendered h3{color:var(--t0);font-weight:600;margin:14px 0 6px;letter-spacing:-.01em}
.sum-rendered h1{font-size:15px}.sum-rendered h2{font-size:14px}.sum-rendered h3{font-size:13.5px}
.sum-rendered p{margin-bottom:10px}
.sum-rendered ul,.sum-rendered ol{margin:6px 0 10px 18px}
.sum-rendered li{margin-bottom:4px}
.sum-rendered strong{color:var(--t0);font-weight:600}
.sum-rendered em{color:var(--t1);font-style:italic}
.sum-rendered hr{border:none;border-top:1px solid var(--border);margin:14px 0}
.sum-meta{font-size:11px;color:var(--t2);font-family:var(--fm);margin-top:12px;padding-top:12px;border-top:1px solid var(--border)}

/* ── History ──────────────────────────── */
.hc{background:var(--bg2);border:1px solid var(--border);border-radius:var(--r);margin-bottom:10px;overflow:hidden;transition:border-color var(--tr)}
.hc:hover{border-color:var(--border2)}
.hh{padding:11px 14px;border-bottom:1px solid var(--border);display:flex;align-items:flex-start;gap:10px}
.hname{font-size:13px;font-weight:600;color:var(--t0);font-family:var(--fm)}
.hts{font-size:11px;color:var(--t2);margin-top:2px}
.hactions{display:flex;gap:6px;flex-shrink:0;align-items:flex-start;flex-wrap:wrap}
.hbody{padding:13px 14px}
.hsum{font-size:13px;line-height:1.65;color:var(--t1);word-break:break-word;max-height:140px;overflow:hidden;position:relative}.hsum .sum-rendered{font-size:13px}
.hsum.expanded{max-height:none}
.hsum.clamped::after{content:'';position:absolute;bottom:0;left:0;right:0;height:38px;background:linear-gradient(transparent,var(--bg2))}
.hfooter{display:flex;gap:6px;margin-top:10px;flex-wrap:wrap}
.hcount{font-size:11px;color:var(--t2);font-family:var(--fm);margin-bottom:12px}
.hloading{text-align:center;padding:36px;color:var(--t2);font-size:13px}
.hempty{text-align:center;padding:44px 16px;color:var(--t2)}
.hempty .ei{font-size:32px;margin-bottom:10px}

/* ── Chat ─────────────────────────────── */
.chat-msgs{flex:1;overflow-y:auto;padding:18px 22px;display:flex;flex-direction:column;gap:12px}
.chat-ctx-banner{background:var(--bg3);border:1px solid var(--border2);border-radius:var(--rsm);padding:9px 14px;font-size:12px;color:var(--t1);display:flex;align-items:center;gap:8px;flex-shrink:0;margin:0 22px 0}
.chat-ctx-banner strong{color:var(--accent2)}
.chat-ctx-banner button{margin-left:auto;background:none;border:none;color:var(--t2);cursor:pointer;font-size:11px;padding:2px 6px;border-radius:4px;transition:color var(--tr)}
.chat-ctx-banner button:hover{color:var(--t0)}
.cw{max-width:420px;margin:auto;text-align:center;padding:36px 16px}
.cw-icon{width:56px;height:56px;background:linear-gradient(135deg,var(--accent),var(--accent2));border-radius:16px;display:flex;align-items:center;justify-content:center;font-size:24px;margin:0 auto 16px}
.cw-title{font-size:18px;font-weight:700;letter-spacing:-.03em;margin-bottom:6px}
.cw-sub{color:var(--t1);font-size:13px}
.sug-grid{display:flex;flex-wrap:wrap;gap:6px;justify-content:center;margin-top:18px}
.sug{padding:6px 12px;border:1px solid var(--border2);border-radius:20px;font-size:12px;color:var(--t1);cursor:pointer;background:var(--bg2);transition:all var(--tr)}
.sug:hover{border-color:var(--accent);color:var(--accent2);background:var(--agl)}
.msg{display:flex;gap:9px;animation:msgin .2s ease}
.msg.user{flex-direction:row-reverse;margin-left:auto;max-width:75%}
.msg.ai{max-width:82%}
@keyframes msgin{from{opacity:0;transform:translateY(6px)}to{opacity:1;transform:none}}
.mav{width:28px;height:28px;border-radius:8px;display:flex;align-items:center;justify-content:center;font-size:12px;flex-shrink:0;margin-top:2px}
.msg.ai .mav{background:linear-gradient(135deg,var(--accent),var(--accent2))}
.msg.user .mav{background:var(--bg3);border:1px solid var(--border2)}
.mbub{padding:10px 13px;border-radius:var(--r);font-size:13.5px;line-height:1.62;word-break:break-word}
.msg.ai .mbub{background:var(--bg2);border:1px solid var(--border);border-radius:3px 10px 10px 10px;color:var(--t0)}
.msg.user .mbub{background:var(--accent);color:#fff;border-radius:10px 3px 10px 10px}
.think{display:flex;align-items:center;gap:4px;padding:11px 14px;background:var(--bg2);border:1px solid var(--border);border-radius:3px 10px 10px 10px}
.tdot{width:6px;height:6px;border-radius:50%;background:var(--t2);animation:tb 1.1s ease infinite}
.tdot:nth-child(2){animation-delay:.14s}.tdot:nth-child(3){animation-delay:.28s}
@keyframes tb{0%,80%,100%{transform:translateY(0);opacity:.3}40%{transform:translateY(-5px);opacity:1}}
.chat-inp-area{padding:12px 22px 16px;border-top:1px solid var(--border);background:var(--bg1);flex-shrink:0}
.cinp-wrap{display:flex;align-items:flex-end;gap:8px;background:var(--bg3);border:1px solid var(--border);border-radius:var(--r);padding:8px 11px;transition:border-color var(--tr)}
.cinp-wrap:focus-within{border-color:var(--accent)}
.cinp{flex:1;background:none;border:none;outline:none;color:var(--t0);font-family:var(--f);font-size:13.5px;line-height:1.5;resize:none;max-height:100px;min-height:20px}
.cinp::placeholder{color:var(--t2)}
.sbtn{width:30px;height:30px;border-radius:7px;background:var(--accent);border:none;cursor:pointer;display:flex;align-items:center;justify-content:center;color:#fff;font-size:13px;flex-shrink:0;transition:all var(--tr)}
.sbtn:hover{background:var(--accent2);transform:scale(1.06)}
.sbtn:disabled{opacity:.3;cursor:not-allowed;transform:none}
.chat-hint{font-size:11px;color:var(--t2);text-align:center;margin-top:6px}

/* ── Settings ─────────────────────────── */
.s-sec{margin-bottom:20px}
.s-lbl{font-size:10px;font-weight:700;text-transform:uppercase;letter-spacing:.06em;color:var(--t2);margin-bottom:8px}
.fgrp{background:var(--bg2);border:1px solid var(--border);border-radius:var(--r);overflow:hidden}
.frow{display:flex;align-items:center;padding:10px 14px;gap:10px;border-bottom:1px solid var(--border)}
.frow:last-child{border-bottom:none}
.flbl{font-size:12.5px;color:var(--t1);width:100px;flex-shrink:0}
.finp{flex:1;background:var(--bg3);border:1px solid var(--border);border-radius:var(--rsm);padding:7px 10px;color:var(--t0);font-family:var(--f);font-size:13px;outline:none;transition:border-color var(--tr)}
.finp:focus{border-color:var(--accent)}
.finp[type=password]{font-family:var(--fm);letter-spacing:.1em}

/* ── Buttons ──────────────────────────── */
.btn{display:inline-flex;align-items:center;justify-content:center;gap:5px;padding:7px 14px;border-radius:var(--rsm);font-size:12.5px;font-weight:500;font-family:var(--f);cursor:pointer;border:none;transition:all var(--tr)}
.btn-p{background:var(--accent);color:#fff}.btn-p:hover{background:var(--accent2)}
.btn-s{background:var(--bg3);color:var(--t1);border:1px solid var(--border)}.btn-s:hover{background:var(--bg4);color:var(--t0)}
.btn-d{background:rgba(224,85,85,0.1);color:var(--red);border:1px solid rgba(224,85,85,0.18)}.btn-d:hover{background:rgba(224,85,85,0.2)}
.btn-ai{background:var(--agl);color:var(--accent2);border:1px solid rgba(124,92,252,0.22)}.btn-ai:hover{background:rgba(124,92,252,0.22)}
.btn-sm{padding:5px 10px;font-size:12px}
.btn:disabled{opacity:.4;cursor:not-allowed}

/* ── Divider headings ─────────────────── */
.tab-head{display:flex;align-items:center;justify-content:space-between;margin-bottom:14px}
.tab-title{font-size:14px;font-weight:700;letter-spacing:-.02em}
.tab-acts{display:flex;gap:7px}

/* ── Toast ────────────────────────────── */
#toasts{position:fixed;top:14px;right:14px;z-index:9999;display:flex;flex-direction:column;gap:6px;pointer-events:none}
.toast{background:var(--bg3);border:1px solid var(--border2);border-radius:var(--rsm);padding:9px 13px;font-size:12.5px;color:var(--t0);box-shadow:var(--sh);display:flex;align-items:center;gap:7px;pointer-events:auto;animation:tin .22s ease}
@keyframes tin{from{opacity:0;transform:translateX(14px)}to{opacity:1;transform:none}}
.toast.ok{border-left:3px solid var(--green)}.toast.err{border-left:3px solid var(--red)}.toast.info{border-left:3px solid var(--blue)}

/* ── Scrollbar ────────────────────────── */
::-webkit-scrollbar{width:4px;height:4px}::-webkit-scrollbar-track{background:transparent}
::-webkit-scrollbar-thumb{background:var(--bg5);border-radius:3px}::-webkit-scrollbar-thumb:hover{background:var(--t2)}

/* ── Status pill in topbar (mobile only) ── */
.tb-pill{display:none;align-items:center;gap:5px;padding:3px 8px;border-radius:20px;font-size:10px;font-weight:600;letter-spacing:.04em;text-transform:uppercase}
.tb-pill.idle{background:rgba(85,85,106,0.18);color:var(--t2)}
.tb-pill.recording{background:rgba(224,85,85,0.13);color:var(--red)}
.tb-pill.processing{background:rgba(74,158,255,0.13);color:var(--blue)}
.tb-pill.done{background:rgba(62,207,122,0.13);color:var(--green)}

/* ── Bottom nav (mobile) ──────────────── */
.bnav{display:none;position:fixed;bottom:0;left:0;right:0;
      background:var(--bg1);border-top:1px solid var(--border);z-index:300;
      align-items:stretch;
      padding-bottom:env(safe-area-inset-bottom,0px);
      /* 62px visible height + safe-area below — clearance above must match */}
.bni{flex:1;display:flex;flex-direction:column;align-items:center;justify-content:center;
     gap:2px;background:none;border:none;color:var(--t2);cursor:pointer;
     padding:6px 0 5px;min-width:0;transition:color var(--tr);position:relative;
     -webkit-tap-highlight-color:transparent}
.bni::before{content:'';position:absolute;top:0;left:50%;transform:translateX(-50%);
             width:0;height:2px;background:var(--accent2);border-radius:0 0 2px 2px;
             transition:width .18s ease}
.bni.active{color:var(--accent2)}.bni.active::before{width:28px}
.bni.locked-bnav{opacity:.32;cursor:not-allowed}
.bni-ico{font-size:20px;line-height:1}
.bni-lbl{font-size:9px;font-weight:600;letter-spacing:.04em;text-transform:uppercase}

@media(max-width:767px){
  .sb{display:none!important}
  .bnav{display:flex}
  .tb-pill{display:inline-flex}
  /* Topbar extends under the transparent status bar on iOS/Android */
  .topbar{
    padding:0 12px;gap:6px;
    padding-top:env(safe-area-inset-top,0px);
    height:calc(48px + env(safe-area-inset-top,0px));
  }
  /* Shrink .main so it ends exactly where the fixed bottom nav begins. */
  .main{padding-bottom:calc(62px + env(safe-area-inset-bottom,0px))}
  /* Individual scroll areas still get a little breathing room            */
  .content{padding:16px 14px 16px!important;overflow-y:auto}
  /* Chat: messages scroll, input bar floats just above the nav           */
  .chat-msgs{padding:14px 14px 14px}
  .chat-inp-area{padding:10px 14px calc(10px + env(safe-area-inset-bottom,0px))!important}
  .chat-ctx-banner{margin:8px 14px 0}
  .stats{grid-template-columns:1fr 1fr}
  /* Compact brand pill on mobile — visible but smaller to fit the narrow topbar */
  .tb-brand{font-size:9.5px;padding:1px 7px;letter-spacing:0}
}
@media(min-width:768px){
  .bnav{display:none!important}
  .tb-pill{display:none!important}
}
/* Tablet (768–1024px): sidebar squeezes content; give it a tiny bottom pad
   so browser chrome on iPadOS doesn't clip the last card.                 */
@media(min-width:768px) and (max-width:1024px){
  .content{padding-bottom:24px}
  .main{padding-bottom:env(safe-area-inset-bottom,0px)}
}
</style>
</head>
<body>
<div class="app">

<!-- ── Sidebar ────────────────────────── -->
<aside class="sb">
  <div class="sb-head">
    <a class="logo" href="/">
      <div class="logo-icon"><svg style="width:16px;height:16px;vertical-align:middle;display:inline-block;flex-shrink:0" width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" xmlns="http://www.w3.org/2000/svg"><rect x="5.5" y="1.5" width="5" height="8" rx="2.5"/><path d="M3 9c0 2.76 2.24 5 5 5s5-2.24 5-5"/><line x1="8" y1="14" x2="8" y2="15.5"/></svg></div>
      MeetRec
    </a>
    <div class="spill idle" id="sPill">
      <div class="sdot"></div><span id="sTxt">Idle</span>
    </div>
  </div>

  <nav class="sb-nav">
    <button class="ni active" data-tab="dashboard">
      <span class="ico"><svg style="width:14px;height:14px;vertical-align:middle;display:inline-block;flex-shrink:0" width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" xmlns="http://www.w3.org/2000/svg"><rect x="1.5" y="1.5" width="5" height="5" rx="1"/><rect x="9.5" y="1.5" width="5" height="5" rx="1"/><rect x="1.5" y="9.5" width="5" height="5" rx="1"/><rect x="9.5" y="9.5" width="5" height="5" rx="1"/></svg></span><span class="lbl">Dashboard</span>
    </button>
    <button class="ni" data-tab="transcript">
      <span class="ico"><svg style="width:14px;height:14px;vertical-align:middle;display:inline-block;flex-shrink:0" width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" xmlns="http://www.w3.org/2000/svg"><path d="M9 1.5H4a1.5 1.5 0 00-1.5 1.5v10A1.5 1.5 0 004 14.5h8A1.5 1.5 0 0013.5 13V6L9 1.5z"/><polyline points="9 1.5 9 6 13.5 6"/><line x1="5.5" y1="9" x2="10.5" y2="9"/><line x1="5.5" y1="11.5" x2="8.5" y2="11.5"/></svg></span><span class="lbl">Transcript</span>
      <span class="nbadge" id="cBadge" style="display:none">0</span>
    </button>
    <button class="ni" data-tab="summary">
      <span class="ico"><svg style="width:14px;height:14px;vertical-align:middle;display:inline-block;flex-shrink:0" width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" xmlns="http://www.w3.org/2000/svg"><path d="M8 2l1.2 2.8L12 6l-2.8 1.2L8 10l-1.2-2.8L4 6l2.8-1.2L8 2z"/><path d="M12.5 10l.6 1.4 1.4.6-1.4.6-.6 1.4-.6-1.4-1.4-.6 1.4-.6.6-1.4z"/></svg></span><span class="lbl">Summary</span>
    </button>
    <button class="ni" data-tab="history" id="histNavBtn">
      <span class="ico"><svg style="width:14px;height:14px;vertical-align:middle;display:inline-block;flex-shrink:0" width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" xmlns="http://www.w3.org/2000/svg"><circle cx="8" cy="8" r="6"/><polyline points="8 5 8 8 10.5 10.5"/></svg></span><span class="lbl">History</span>
      <span class="nbadge" id="histBadge" style="display:none">0</span>
    </button>
    <button class="ni locked" id="chatNavBtn" data-tab="chat">
      <span class="ico"><svg style="width:14px;height:14px;vertical-align:middle;display:inline-block;flex-shrink:0" width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" xmlns="http://www.w3.org/2000/svg"><path d="M13.5 10a1.5 1.5 0 01-1.5 1.5H5L2 14.5V3.5A1.5 1.5 0 013.5 2h9A1.5 1.5 0 0114 3.5v6.5z"/></svg></span><span class="lbl">Ask AI</span>
      <span id="chatLock" style="margin-left:auto;opacity:.4"><svg style="width:11px;height:11px;vertical-align:middle;display:inline-block;flex-shrink:0" width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" xmlns="http://www.w3.org/2000/svg"><rect x="3.5" y="7" width="9" height="7.5" rx="1.5"/><path d="M5.5 7V5.5a2.5 2.5 0 015 0V7"/></svg></span>
    </button>
    <button class="ni" data-tab="settings">
      <span class="ico"><svg style="width:14px;height:14px;vertical-align:middle;display:inline-block;flex-shrink:0" width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" xmlns="http://www.w3.org/2000/svg"><circle cx="8" cy="8" r="2.5"/><path d="M12.9 5.5l-.8-1-1.3.5a4.5 4.5 0 00-1.1-.65L9.4 3H6.6l-.3 1.35a4.5 4.5 0 00-1.1.65L3.9 4.5l-.8 1 .8 1a4.5 4.5 0 000 1.5l-.8 1 .8 1 1.3-.5a4.5 4.5 0 001.1.65L6.6 12h2.8l.3-1.35a4.5 4.5 0 001.1-.65l1.3.5.8-1-.8-1a4.5 4.5 0 000-1.5l.8-1z"/></svg></span><span class="lbl">Settings</span>
    </button>
  </nav>

  <div class="sb-foot">
    <div class="sb-meta">
      <div id="sbTime">&#8212;</div>
      <div id="sbMeta" style="margin-top:1px"></div>
    </div>
  </div>
</aside>

<!-- ── Main ───────────────────────────── -->
<div class="main">
  <div class="topbar">
    <span class="tb-title" id="pgTitle">Dashboard</span>
    <div class="tb-pill idle" id="sPillMob"><div class="sdot"></div><span id="sTxtMob">Idle</span></div>
    <div class="tb-right">
      <a class="tb-brand" href="https://www.youtube.com/@techiesms" target="_blank" rel="noopener noreferrer" title="Visit techiesms on YouTube">techiesms</a>
      <div id="wvf" class="wvf" style="display:none">
        <div class="wb"></div><div class="wb"></div><div class="wb"></div>
        <div class="wb"></div><div class="wb"></div>
      </div>
      <span class="tb-chip" id="tbChip"></span>
    </div>
  </div>

  <!-- ─── Dashboard ─── -->
  <div class="content tab active" id="tab-dashboard">
    <button class="rec-btn start" id="recBtn" onclick="toggleRec()">
      <svg style="width:14px;height:14px;vertical-align:middle;display:inline-block;flex-shrink:0" width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" xmlns="http://www.w3.org/2000/svg"><polygon points="4 2 14 8 4 14" fill="currentColor" stroke="none"/></svg> Start Meeting
    </button>

    <div class="stats">
      <div class="sc"><div class="sl">Duration</div><div class="sv" id="stDur">&#8212;</div><div class="su">elapsed</div></div>
      <div class="sc"><div class="sl">Chunks</div><div class="sv" id="stChunks">0</div><div class="su">processed</div></div>
      <div class="sc"><div class="sl">Words</div><div class="sv" id="stWords">0</div><div class="su">transcribed</div></div>
      <div class="sc"><div class="sl">Status</div><div class="sv" id="stIcon" style="font-size:18px;margin-top:5px">&#8212;</div><div class="su" id="stSub">waiting</div></div>
    </div>

    <div class="card">
      <div class="ch">
        <span class="ct">Live Summary</span>
        <div class="lbadge" id="liveBadge" style="display:none"><div class="ldot"></div> Live</div>
      </div>
      <div class="cb">
        <div id="sumLive" class="prose">
          <div class="empty-state"><div class="ei"><svg style="width:28px;height:28px;vertical-align:middle;display:inline-block;flex-shrink:0" width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" xmlns="http://www.w3.org/2000/svg"><rect x="5.5" y="1.5" width="5" height="8" rx="2.5"/><path d="M3 9c0 2.76 2.24 5 5 5s5-2.24 5-5"/><line x1="8" y1="14" x2="8" y2="15.5"/></svg></div><p>Start a meeting — a rolling summary will appear here after the first chunk is processed.</p></div>
        </div>
      </div>
    </div>

    <div class="card">
      <div class="ch"><span class="ct">Recent Transcript</span></div>
      <div class="cb">
        <div id="txPreview" class="prose" style="font-family:var(--fm);font-size:12px;max-height:180px;overflow-y:auto;color:var(--t2)">
          <div class="empty-state" style="padding:12px"><p>No transcript yet.</p></div>
        </div>
      </div>
    </div>
  </div>

  <!-- ─── Transcript ─── -->
  <div class="content tab" id="tab-transcript">
    <div class="tab-head">
      <span class="tab-title">Transcript</span>
      <div class="tab-acts">
        <button class="btn btn-s btn-sm" onclick="copyTx()"><svg style="width:12px;height:12px;vertical-align:middle;display:inline-block;flex-shrink:0" width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" xmlns="http://www.w3.org/2000/svg"><rect x="9" y="2" width="5" height="5" rx="1"/><path d="M7 5H3.5A1.5 1.5 0 002 6.5v7A1.5 1.5 0 003.5 15h7A1.5 1.5 0 0012 13.5V10"/></svg> Copy</button>
      </div>
    </div>
    <div class="tx-banner" id="txBannerLive"><div class="sdot" style="background:var(--red)"></div> Live transcript — building in real time as chunks are processed.</div>
    <div class="tx-banner" id="txBannerFinal"><svg style="width:13px;height:13px;vertical-align:middle;display:inline-block;flex-shrink:0" width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" xmlns="http://www.w3.org/2000/svg"><polyline points="2.5 8 6 11.5 13.5 4.5"/></svg> Final transcript — complete recording saved to SD card.</div>
    <div class="tx-banner" id="txBannerTrunc"><svg style="width:13px;height:13px;vertical-align:middle;display:inline-block;flex-shrink:0" width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" xmlns="http://www.w3.org/2000/svg"><path d="M8 2L14.5 13.5H1.5L8 2z"/><line x1="8" y1="7" x2="8" y2="10"/><circle cx="8" cy="12" r=".6" fill="currentColor" stroke="none"/></svg> Showing a partial transcript. The full version is saved on the SD card (RAM limit reached).</div>
    <div class="tx-box" id="txFull"><span class="tx-empty">No transcript yet. Start a meeting to begin recording.</span></div>
  </div>

  <!-- ─── Summary ─── -->
  <div class="content tab" id="tab-summary">
    <div class="tab-head">
      <span class="tab-title">Meeting Summary</span>
      <div class="tab-acts">
        <button class="btn btn-s btn-sm" id="dlBtn" style="display:none" onclick="dlSummary()"><svg style="width:12px;height:12px;vertical-align:middle;display:inline-block;flex-shrink:0" width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" xmlns="http://www.w3.org/2000/svg"><path d="M8 2.5v8M5.5 8l2.5 2.5 2.5-2.5"/><path d="M3 13.5h10"/></svg> Download</button>
        <button class="btn btn-s btn-sm" onclick="copySum()"><svg style="width:12px;height:12px;vertical-align:middle;display:inline-block;flex-shrink:0" width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" xmlns="http://www.w3.org/2000/svg"><rect x="9" y="2" width="5" height="5" rx="1"/><path d="M7 5H3.5A1.5 1.5 0 002 6.5v7A1.5 1.5 0 003.5 15h7A1.5 1.5 0 0012 13.5V10"/></svg> Copy</button>
      </div>
    </div>
    <div class="card">
      <div class="cb">
        <div id="sumFinal" class="sum-rendered">
          <div class="empty-state"><div class="ei"><svg style="width:28px;height:28px;vertical-align:middle;display:inline-block;flex-shrink:0" width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" xmlns="http://www.w3.org/2000/svg"><path d="M8 2l1.2 2.8L12 6l-2.8 1.2L8 10l-1.2-2.8L4 6l2.8-1.2L8 2z"/><path d="M12.5 10l.6 1.4 1.4.6-1.4.6-.6 1.4-.6-1.4-1.4-.6 1.4-.6.6-1.4z"/></svg></div><p>The final summary will appear here when the meeting ends.</p></div>
        </div>
      </div>
    </div>
    <div id="sumMeta" class="sum-meta" style="display:none"></div>
  </div>

  <!-- ─── History ─── -->
  <div class="content tab" id="tab-history">
    <div class="tab-head">
      <span class="tab-title">Meeting History</span>
      <button class="btn btn-s btn-sm" onclick="loadHistory(true)"><svg style="width:12px;height:12px;vertical-align:middle;display:inline-block;flex-shrink:0" width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" xmlns="http://www.w3.org/2000/svg"><path d="M13 8A5 5 0 113 8"/><polyline points="13 4.5 13 8 9.5 8"/></svg> Refresh</button>
    </div>
    <div id="histList">
      <div class="hempty"><div class="ei"><svg style="width:32px;height:32px;vertical-align:middle;display:inline-block;flex-shrink:0" width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" xmlns="http://www.w3.org/2000/svg"><circle cx="8" cy="8" r="6"/><polyline points="8 5 8 8 10.5 10.5"/></svg></div><p>Your past meetings will appear here.</p></div>
    </div>
  </div>

  <!-- ─── Chat ─── -->
  <div class="chat-wrap" id="tab-chat">
    <div class="chat-ctx-banner" id="chatCtxBanner" style="display:none">
      <span><svg style="width:13px;height:13px;vertical-align:middle;display:inline-block;flex-shrink:0" width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" xmlns="http://www.w3.org/2000/svg"><circle cx="8" cy="8" r="6"/><polyline points="8 5 8 8 10.5 10.5"/></svg></span>
      <span>Asking about: <strong id="chatCtxName"></strong></span>
      <button onclick="clearHistCtx()">&#215; Use current meeting</button>
    </div>
    <div class="chat-msgs" id="chatMsgs">
      <div class="cw" id="chatWelcome">
        <div class="cw-icon"><svg style="width:24px;height:24px;vertical-align:middle;display:inline-block;flex-shrink:0" width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" xmlns="http://www.w3.org/2000/svg"><path d="M8 2l1.2 2.8L12 6l-2.8 1.2L8 10l-1.2-2.8L4 6l2.8-1.2L8 2z"/><path d="M12.5 10l.6 1.4 1.4.6-1.4.6-.6 1.4-.6-1.4-1.4-.6 1.4-.6.6-1.4z"/></svg></div>
        <div class="cw-title">Ask about your meeting</div>
        <div class="cw-sub">I have the full context — ask about decisions, action items, who said what, or anything discussed.</div>
        <div class="sug-grid">
          <div class="sug" onclick="askSug(this)">What were the key decisions?</div>
          <div class="sug" onclick="askSug(this)">List all action items</div>
          <div class="sug" onclick="askSug(this)">Who is responsible for what?</div>
          <div class="sug" onclick="askSug(this)">What problems were raised?</div>
          <div class="sug" onclick="askSug(this)">Draft a follow-up email</div>
          <div class="sug" onclick="askSug(this)">Summarise in 3 bullets</div>
        </div>
      </div>
    </div>
    <div class="chat-inp-area">
      <div class="cinp-wrap">
        <textarea class="cinp" id="chatInp" rows="1" placeholder="Ask anything about the meeting&#8230;" onkeydown="chatKey(event)" oninput="autoH(this)"></textarea>
        <button class="sbtn" id="sendBtn" onclick="sendChat()"><svg style="width:13px;height:13px;vertical-align:middle;display:inline-block;flex-shrink:0" width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" xmlns="http://www.w3.org/2000/svg"><line x1="2" y1="8" x2="13" y2="8"/><polyline points="9 4 13 8 9 12"/></svg></button>
      </div>
      <div class="chat-hint">Powered by GPT &middot; Enter to send, Shift+Enter for new line</div>
    </div>
  </div>

  <!-- ─── Settings ─── -->
  <div class="content tab" id="tab-settings">
    <div class="tab-head"><span class="tab-title">Settings</span></div>

    <div class="s-sec">
      <div class="s-lbl">WiFi Network</div>
      <div class="fgrp">
        <div class="frow"><div class="flbl">SSID</div><input class="finp" id="cSSID" type="text" placeholder="Network name"></div>
        <div class="frow"><div class="flbl">Password</div><input class="finp" id="cPass" type="password" placeholder="&bull;&bull;&bull;&bull;&bull;&bull;&bull;&bull;"></div>
      </div>
    </div>

    <div class="s-sec">
      <div class="s-lbl">API Keys</div>
      <div class="fgrp">
        <div class="frow"><div class="flbl">ElevenLabs</div><input class="finp" id="cEL" type="password" placeholder="el_&hellip;"></div>
        <div class="frow"><div class="flbl">OpenAI</div><input class="finp" id="cOAI" type="password" placeholder="sk-&hellip;"></div>
      </div>
    </div>

    <div class="s-sec">
      <div class="s-lbl">Timezone</div>
      <div class="fgrp">
        <div class="frow">
          <div class="flbl">Time Zone</div>
          <select class="finp" id="cTZ">
            <option value="-720">UTC&minus;12:00 &mdash; Baker Island</option>
            <option value="-660">UTC&minus;11:00 &mdash; American Samoa</option>
            <option value="-600">UTC&minus;10:00 &mdash; Hawaii</option>
            <option value="-540">UTC&minus;09:00 &mdash; Alaska</option>
            <option value="-480">UTC&minus;08:00 &mdash; Pacific (PST)</option>
            <option value="-420">UTC&minus;07:00 &mdash; Mountain (MST)</option>
            <option value="-360">UTC&minus;06:00 &mdash; Central (CST)</option>
            <option value="-300">UTC&minus;05:00 &mdash; Eastern (EST)</option>
            <option value="-240">UTC&minus;04:00 &mdash; Atlantic</option>
            <option value="-180">UTC&minus;03:00 &mdash; Argentina, Brazil</option>
            <option value="-120">UTC&minus;02:00 &mdash; South Georgia</option>
            <option value="-60">UTC&minus;01:00 &mdash; Azores</option>
            <option value="0">UTC&plusmn;00:00 &mdash; London, Lisbon (GMT)</option>
            <option value="60">UTC+01:00 &mdash; Berlin, Paris (CET)</option>
            <option value="120">UTC+02:00 &mdash; Athens, Cairo (EET)</option>
            <option value="180">UTC+03:00 &mdash; Moscow, Riyadh</option>
            <option value="210">UTC+03:30 &mdash; Tehran</option>
            <option value="240">UTC+04:00 &mdash; Dubai</option>
            <option value="270">UTC+04:30 &mdash; Kabul</option>
            <option value="300">UTC+05:00 &mdash; Karachi, Tashkent</option>
            <option value="330" selected>UTC+05:30 &mdash; India (IST), Sri Lanka</option>
            <option value="345">UTC+05:45 &mdash; Kathmandu</option>
            <option value="360">UTC+06:00 &mdash; Dhaka</option>
            <option value="390">UTC+06:30 &mdash; Yangon</option>
            <option value="420">UTC+07:00 &mdash; Bangkok, Jakarta</option>
            <option value="480">UTC+08:00 &mdash; Beijing, Singapore</option>
            <option value="540">UTC+09:00 &mdash; Tokyo, Seoul (JST)</option>
            <option value="570">UTC+09:30 &mdash; Adelaide</option>
            <option value="600">UTC+10:00 &mdash; Sydney</option>
            <option value="660">UTC+11:00 &mdash; Solomon Islands</option>
            <option value="720">UTC+12:00 &mdash; New Zealand</option>
          </select>
        </div>
      </div>
      <div style="font-size:11px;color:var(--t2);margin-top:6px">Used for timestamping meeting folders and the display clock.</div>
    </div>

    <div style="display:flex;gap:8px;margin-top:16px">
      <button class="btn btn-p" onclick="saveCfg()">Save &amp; Apply</button>
      <button class="btn btn-s" onclick="loadCfg()">Clear Fields</button>
    </div>

    <div class="s-sec" style="margin-top:28px">
      <div class="s-lbl">Device Info</div>
      <div class="fgrp">
        <div class="frow"><div class="flbl">Firmware</div><span style="font-family:var(--fm);font-size:12px;color:var(--t1)">MeetingRecorder</span></div>
        <div class="frow"><div class="flbl">Free RAM</div><span class="finp-val" id="aRam">&#8212;</span></div>
        <div class="frow"><div class="flbl">Uptime</div><span class="finp-val" id="aUp">&#8212;</span></div>
        <div class="frow"><div class="flbl">NTP Synced</div><span class="finp-val" id="aNtp">&#8212;</span></div>
        <div class="frow"><div class="flbl">Last Meeting</div><span class="finp-val" id="aFile">&#8212;</span></div>
      </div>
    </div>

    <div class="s-sec" style="margin-top:32px;border:1px solid rgba(248,81,73,0.25);background:rgba(248,81,73,0.04);border-radius:var(--rsm);padding:14px 16px">
      <div class="s-lbl" style="color:var(--red)">Danger Zone</div>
      <div style="font-size:12px;color:var(--t2);line-height:1.55;margin-bottom:10px">
        Factory reset wipes <strong>all saved meetings</strong> (transcripts, audio, summaries) and <strong>all credentials</strong> (WiFi, API keys, AP settings). The device reboots into setup mode &mdash; you will need to reconnect to its hotspot and configure everything from scratch.
      </div>
      <button class="btn btn-d" onclick="factoryReset()" style="width:auto">
        <svg style="width:13px;height:13px;vertical-align:middle;display:inline-block;flex-shrink:0;margin-right:5px" width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" xmlns="http://www.w3.org/2000/svg"><path d="M8 2L14.5 13.5H1.5L8 2z"/><line x1="8" y1="7" x2="8" y2="10"/><circle cx="8" cy="12" r=".6" fill="currentColor" stroke="none"/></svg>
        Factory Reset
      </button>
    </div>
  </div>
</div><!-- .main -->

<!-- Bottom Nav (mobile only) -->
<nav class="bnav" id="bNav">
  <button class="bni active" data-tab="dashboard" onclick="goTab('dashboard')">
    <span class="bni-ico"><svg style="width:18px;height:18px;vertical-align:middle;display:inline-block;flex-shrink:0" width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" xmlns="http://www.w3.org/2000/svg"><rect x="1.5" y="1.5" width="5" height="5" rx="1"/><rect x="9.5" y="1.5" width="5" height="5" rx="1"/><rect x="1.5" y="9.5" width="5" height="5" rx="1"/><rect x="9.5" y="9.5" width="5" height="5" rx="1"/></svg></span><span class="bni-lbl">Home</span>
  </button>
  <button class="bni" data-tab="transcript" onclick="goTab('transcript')">
    <span class="bni-ico"><svg style="width:18px;height:18px;vertical-align:middle;display:inline-block;flex-shrink:0" width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" xmlns="http://www.w3.org/2000/svg"><path d="M9 1.5H4a1.5 1.5 0 00-1.5 1.5v10A1.5 1.5 0 004 14.5h8A1.5 1.5 0 0013.5 13V6L9 1.5z"/><polyline points="9 1.5 9 6 13.5 6"/><line x1="5.5" y1="9" x2="10.5" y2="9"/><line x1="5.5" y1="11.5" x2="8.5" y2="11.5"/></svg></span><span class="bni-lbl">Text</span>
  </button>
  <button class="bni" data-tab="summary" onclick="goTab('summary')">
    <span class="bni-ico"><svg style="width:18px;height:18px;vertical-align:middle;display:inline-block;flex-shrink:0" width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" xmlns="http://www.w3.org/2000/svg"><path d="M8 2l1.2 2.8L12 6l-2.8 1.2L8 10l-1.2-2.8L4 6l2.8-1.2L8 2z"/><path d="M12.5 10l.6 1.4 1.4.6-1.4.6-.6 1.4-.6-1.4-1.4-.6 1.4-.6.6-1.4z"/></svg></span><span class="bni-lbl">Summary</span>
  </button>
  <button class="bni" data-tab="history" onclick="goTab('history')">
    <span class="bni-ico"><svg style="width:18px;height:18px;vertical-align:middle;display:inline-block;flex-shrink:0" width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" xmlns="http://www.w3.org/2000/svg"><circle cx="8" cy="8" r="6"/><polyline points="8 5 8 8 10.5 10.5"/></svg></span><span class="bni-lbl">History</span>
  </button>
  <button class="bni locked-bnav" id="bniChat" data-tab="chat" onclick="goTab('chat')">
    <span class="bni-ico"><svg style="width:18px;height:18px;vertical-align:middle;display:inline-block;flex-shrink:0" width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" xmlns="http://www.w3.org/2000/svg"><path d="M13.5 10a1.5 1.5 0 01-1.5 1.5H5L2 14.5V3.5A1.5 1.5 0 013.5 2h9A1.5 1.5 0 0114 3.5v6.5z"/></svg></span><span class="bni-lbl">Ask AI</span>
  </button>
  <button class="bni" data-tab="settings" onclick="goTab('settings')">
    <span class="bni-ico"><svg style="width:18px;height:18px;vertical-align:middle;display:inline-block;flex-shrink:0" width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" xmlns="http://www.w3.org/2000/svg"><circle cx="8" cy="8" r="2.5"/><path d="M12.9 5.5l-.8-1-1.3.5a4.5 4.5 0 00-1.1-.65L9.4 3H6.6l-.3 1.35a4.5 4.5 0 00-1.1.65L3.9 4.5l-.8 1 .8 1a4.5 4.5 0 000 1.5l-.8 1 .8 1 1.3-.5a4.5 4.5 0 001.1.65L6.6 12h2.8l.3-1.35a4.5 4.5 0 001.1-.65l1.3.5.8-1-.8-1a4.5 4.5 0 000-1.5l.8-1z"/></svg></span><span class="bni-lbl">More</span>
  </button>
</nav>
</div><!-- .app -->

<div id="toasts"></div>

<script>
/* ── State ───────────────────────────── */
var tab='dashboard', prevState='', meetingStart=null, durTimer=null,
    chatHist=[], summaryReady=false, recBusy=false,
    histLoaded=false, histItems=[],
    chatMode='current', histCtxSummary='', histCtxName='';

/* ── Simple markdown → HTML renderer ── */
function renderMd(text){
  if(!text||!text.trim()) return '';
  var lines=text.split('\n');
  var html='';var inUl=false;var inOl=false;
  lines.forEach(function(line){
    var t=line.trimRight();
    // headings
    if(/^### /.test(t)){if(inUl){html+='</ul>';inUl=false}if(inOl){html+='</ol>';inOl=false}html+='<h3>'+esc(t.slice(4))+'</h3>';return}
    if(/^## /.test(t)){if(inUl){html+='</ul>';inUl=false}if(inOl){html+='</ol>';inOl=false}html+='<h2>'+esc(t.slice(3))+'</h2>';return}
    if(/^# /.test(t)){if(inUl){html+='</ul>';inUl=false}if(inOl){html+='</ol>';inOl=false}html+='<h1>'+esc(t.slice(2))+'</h1>';return}
    // numbered list
    if(/^\d+\. /.test(t)){if(inUl){html+='</ul>';inUl=false}if(!inOl){html+='<ol>';inOl=true}html+='<li>'+inlineMd(esc(t.replace(/^\d+\. /,'')))+'</li>';return}
    // bullet
    if(/^[-*] /.test(t)){if(inOl){html+='</ol>';inOl=false}if(!inUl){html+='<ul>';inUl=true}html+='<li>'+inlineMd(esc(t.slice(2)))+'</li>';return}
    // close lists
    if(inUl){html+='</ul>';inUl=false}
    if(inOl){html+='</ol>';inOl=false}
    // HR
    if(/^---+$/.test(t)){html+='<hr>';return}
    // blank line
    if(t.length===0){html+='<p style="margin:4px 0"></p>';return}
    html+='<p>'+inlineMd(esc(t))+'</p>';
  });
  if(inUl) html+='</ul>';
  if(inOl) html+='</ol>';
  return html;
}
function inlineMd(s){
  return s
    .replace(/\*\*(.+?)\*\*/g,'<strong>$1</strong>')
    .replace(/\*(.+?)\*/g,'<em>$1</em>')
    .replace(/__(.+?)__/g,'<strong>$1</strong>');
}
function esc(s){return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;')}
function escHtml(s){return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/\n/g,'<br>')}
function renderAns(s){
  if(s.indexOf('[ERR] ')===0){
    var msg=esc(s.substring(6));
    var icon='<svg width="15" height="15" viewBox="0 0 16 16" fill="none" xmlns="http://www.w3.org/2000/svg" style="flex-shrink:0;margin-top:1px"><path d="M8 1.5 14.8 13.5H1.2Z" stroke="currentColor" stroke-width="1.4" stroke-linejoin="round"/><line x1="8" y1="6" x2="8" y2="9.5" stroke="currentColor" stroke-width="1.4" stroke-linecap="round"/><circle cx="8" cy="11.5" r=".65" fill="currentColor"/></svg>';
    return '<span style="display:inline-flex;align-items:flex-start;gap:7px;color:var(--red);font-size:13px;line-height:1.55">'+icon+'<span>'+msg+'</span></span>';
  }
  return escHtml(s);
}

/* ── Navigation ──────────────────────── */
var TITLES={dashboard:'Dashboard',transcript:'Transcript',summary:'Summary',history:'History',chat:'Ask AI',settings:'Settings'};
document.querySelectorAll('.ni[data-tab]').forEach(function(b){b.addEventListener('click',function(){goTab(b.dataset.tab)})});

function goTab(id){
  if(id==='chat'&&!summaryReady&&chatMode!=='history'){
    toast('Complete a meeting first to unlock Ask AI','info');
    return;
  }
  document.querySelectorAll('.tab,.chat-wrap').forEach(function(p){p.classList.remove('active')});
  document.querySelectorAll('.ni,.bni').forEach(function(n){n.classList.remove('active')});
  var pg=document.getElementById('tab-'+id);
  if(pg) pg.classList.add('active');
  document.querySelectorAll('.ni[data-tab="'+id+'"],.bni[data-tab="'+id+'"]').forEach(function(n){n.classList.add('active')});
  document.getElementById('pgTitle').textContent=TITLES[id]||id;
  tab=id;
  if(id==='history'&&!histLoaded) loadHistory(false);
}

/* ── Toast ───────────────────────────── */
function toast(msg,type){
  type=type||'info';
  var c=document.getElementById('toasts');
  var t=document.createElement('div');
  t.className='toast '+type;
  var _ti={'ok':'<svg style="width:12px;height:12px;vertical-align:middle" viewBox="0 0 16 16" fill="none" stroke="var(--green)" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><polyline points="2.5 8 6 11.5 13.5 4.5"/></svg>','err':'<svg style="width:12px;height:12px;vertical-align:middle" viewBox="0 0 16 16" fill="none" stroke="var(--red)" stroke-width="2" stroke-linecap="round"><line x1="4" y1="4" x2="12" y2="12"/><line x1="12" y1="4" x2="4" y2="12"/></svg>','info':'<svg style="width:12px;height:12px;vertical-align:middle" viewBox="0 0 16 16" fill="none" stroke="var(--blue)" stroke-width="2" stroke-linecap="round"><circle cx="8" cy="8" r="6"/><line x1="8" y1="7" x2="8" y2="11"/><circle cx="8" cy="5" r=".5" fill="var(--blue)" stroke="none"/></svg>'};t.innerHTML=(_ti[type]||'')+' '+msg;
  c.appendChild(t);
  setTimeout(function(){t.style.cssText='opacity:0;transition:.28s';setTimeout(function(){t.remove()},280)},3600);
}

/* ── Record toggle ───────────────────── */
async function toggleRec(){
  if(recBusy) return;
  recBusy=true;
  var btn=document.getElementById('recBtn');
  btn.disabled=true;
  var isRec=btn.classList.contains('stop');
  try{
    var r=await fetch(isRec?'/api/stop':'/api/start',{method:'POST'});
    if(!r.ok) throw new Error('HTTP '+r.status);
    if(!isRec){
      // Reset transcript tab for new meeting
      document.getElementById('txFull').innerHTML='<span class="tx-empty">Recording started&#8230;</span>';
      document.getElementById('txBannerFinal').style.display='none';
      document.getElementById('txBannerTrunc').style.display='none';
    }
    toast(isRec?'Meeting stopped — generating final summary&#8230;':'Meeting started!','ok');
  }catch(e){toast('Error — could not reach device','err')}
  btn.disabled=false;recBusy=false;
}

/* ── Polling ─────────────────────────── */
/* Skip polling while the tab is in the background — saves device RAM
   (each /api/status response is ~4 KB) and the user's mobile battery.
   Triggers a single fresh poll the moment the tab is foregrounded again. */
function poll(){
  if(document.hidden) return;
  fetch('/api/status').then(function(r){return r.json()}).then(updateUI).catch(function(){});
}
document.addEventListener('visibilitychange',function(){if(!document.hidden) poll()});

function updateUI(d){
  var state=d.state||'idle';

  /* status pill (sidebar + mobile topbar) */
  var stateLabel={idle:'Idle',recording:'Recording',processing:'Processing',done:'Done'};
  var pill=document.getElementById('sPill');
  pill.className='spill '+state;
  document.getElementById('sTxt').textContent=stateLabel[state]||state;
  var pillMob=document.getElementById('sPillMob');
  pillMob.className='tb-pill '+state;
  document.getElementById('sTxtMob').textContent=stateLabel[state]||state;

  /* record button */
  var btn=document.getElementById('recBtn');
  if(!btn.disabled){
    if(state==='recording'){btn.className='rec-btn stop';btn.innerHTML='<svg style="width:12px;height:12px;vertical-align:middle;display:inline-block;flex-shrink:0" width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" xmlns="http://www.w3.org/2000/svg"><rect x="3" y="3" width="10" height="10" rx="1.5" fill="currentColor" stroke="none"/></svg> Stop Meeting'}
    else{btn.className='rec-btn start';btn.innerHTML='<svg style="width:14px;height:14px;vertical-align:middle;display:inline-block;flex-shrink:0" width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" xmlns="http://www.w3.org/2000/svg"><polygon points="4 2 14 8 4 14" fill="currentColor" stroke="none"/></svg> Start Meeting'}
  }

  /* waveform + live badge */
  document.getElementById('wvf').style.display=state==='recording'?'flex':'none';
  document.getElementById('liveBadge').style.display=state==='recording'?'inline-flex':'none';
  document.getElementById('tbChip').textContent=state==='recording'?'● REC':'';

  /* stats */
  document.getElementById('stChunks').textContent=d.chunks||0;
  var wc=d.wordCount||0;
  document.getElementById('stWords').textContent=wc>999?(wc/1000).toFixed(1)+'k':wc;
  var icons={idle:'&mdash;',recording:'<svg style="width:10px;height:10px;display:inline-block;vertical-align:middle" viewBox="0 0 10 10"><circle cx="5" cy="5" r="4" fill="var(--red)"/></svg>',processing:'<svg style="width:14px;height:14px;display:inline-block;vertical-align:middle" viewBox="0 0 16 16" fill="none" stroke="var(--blue)" stroke-width="1.5" stroke-linecap="round"><path d="M8 2a6 6 0 100 12A6 6 0 008 2z" opacity=".25"/><path d="M14 8a6 6 0 00-6-6"><animateTransform attributeName="transform" type="rotate" from="0 8 8" to="360 8 8" dur="1s" repeatCount="indefinite"/></path></svg>',done:'<svg style="width:14px;height:14px;display:inline-block;vertical-align:middle" viewBox="0 0 16 16" fill="none" stroke="var(--green)" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"><polyline points="2.5 8 6 11.5 13.5 4.5"/></svg>'};
  var subs={idle:'waiting',recording:'capturing',processing:'transcribing',done:'complete'};
  document.getElementById('stIcon').innerHTML=icons[state]||'&#8212;';
  document.getElementById('stSub').textContent=subs[state]||'';

  var badge=document.getElementById('cBadge');
  if(d.chunks>0){badge.textContent=d.chunks;badge.style.display='inline'}

  /* sidebar meta */
  if(d.meetingTime) document.getElementById('sbTime').textContent=d.meetingTime;
  if(d.chunks) document.getElementById('sbMeta').textContent=d.chunks+' chunk'+(d.chunks===1?'':'s');

  /* device info */
  if(d.freeRam) document.getElementById('aRam').textContent=Math.round(d.freeRam/1024)+' KB free';
  if(d.uptime) document.getElementById('aUp').textContent=fmtUp(d.uptime);
  document.getElementById('aNtp').textContent=d.ntpSynced?'Yes ✓':'No (browser time)';
  if(d.summaryFile&&d.summaryFile.length>2) document.getElementById('aFile').textContent=d.summaryFile;

  /* Sync timezone dropdown to the device's saved value — but only on
     first poll, so the user can change the selection without us
     overwriting it on the next /api/status response. */
  if(typeof d.tzMin==='number' && !window._tzSynced){
    var sel=document.getElementById('cTZ');
    if(sel){ sel.value=String(d.tzMin); window._tzSynced=true; }
  }

  /* duration timer */
  if(state==='recording'&&!meetingStart){
    meetingStart=Date.now()-(d.elapsedSeconds||0)*1000;
    durTimer=setInterval(tickDur,1000);
  }else if(state!=='recording'&&meetingStart){
    clearInterval(durTimer);
    if(state==='idle') meetingStart=null;
  }

  /* reset state when new recording starts */
  if(state==='recording'&&prevState!==''&&prevState!=='recording'&&summaryReady){
    summaryReady=false;chatHist=[];chatMode='current';histCtxSummary='';histCtxName='';
    var nb=document.getElementById('chatNavBtn');
    nb.classList.add('locked');
    if(!document.getElementById('chatLock')){
      var lk=document.createElement('span');lk.id='chatLock';
      lk.style.cssText='margin-left:auto;font-size:10px;opacity:.5';lk.innerHTML='<svg style="width:11px;height:11px;vertical-align:middle;display:inline-block;flex-shrink:0" width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" xmlns="http://www.w3.org/2000/svg"><rect x="3.5" y="7" width="9" height="7.5" rx="1.5"/><path d="M5.5 7V5.5a2.5 2.5 0 015 0V7"/></svg>';
      nb.appendChild(lk);
    }
    var bni=document.getElementById('bniChat');if(bni)bni.classList.add('locked-bnav');
    document.getElementById('sumFinal').innerHTML='<div class="empty-state"><div class="ei"><svg style="width:28px;height:28px;vertical-align:middle;display:inline-block;flex-shrink:0" width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" xmlns="http://www.w3.org/2000/svg"><path d="M8 2l1.2 2.8L12 6l-2.8 1.2L8 10l-1.2-2.8L4 6l2.8-1.2L8 2z"/><path d="M12.5 10l.6 1.4 1.4.6-1.4.6-.6 1.4-.6-1.4-1.4-.6 1.4-.6.6-1.4z"/></svg></div><p>The final summary will appear here when the meeting ends.</p></div>';
    document.getElementById('dlBtn').style.display='none';
    var sm=document.getElementById('sumMeta');sm.style.display='none';sm.textContent='';
  }

  /* rolling summary — render markdown so bold/lists look the same as Summary tab */
  if(d.rollingSummary&&d.rollingSummary.trim().length>2){
    var sl=document.getElementById('sumLive');
    if(sl.dataset.src!==d.rollingSummary){
      sl.dataset.src=d.rollingSummary;
      sl.innerHTML=renderMd(d.rollingSummary)||esc(d.rollingSummary);
    }
  }

  /* ── Transcript tab logic ── */
  var txEl=document.getElementById('txFull');
  var bLive=document.getElementById('txBannerLive');
  var bFinal=document.getElementById('txBannerFinal');
  var bTrunc=document.getElementById('txBannerTrunc');

  if(d.finalTranscript&&d.finalTranscript.trim().length>3){
    // Meeting ended — show final (or partial) transcript snapshot
    if(txEl.dataset.mode!=='final'||(txEl.dataset.content||'')!==d.finalTranscript){
      txEl.dataset.mode='final';
      txEl.dataset.content=d.finalTranscript;
      txEl.textContent=d.finalTranscript;
    }
    bLive.style.display='none';
    bFinal.style.display='flex';
    // Show truncation notice if content looks trimmed (over ~5500 chars)
    bTrunc.style.display=d.finalTranscript.length>=5500?'flex':'none';
    // Dashboard preview
    document.getElementById('txPreview').textContent=d.finalTranscript.slice(-600);
  } else if(d.transcript&&d.transcript.trim().length>3){
    // Meeting in progress — show live rolling transcript
    if(txEl.dataset.mode!=='live'||(txEl.dataset.content||'')!==d.transcript){
      txEl.dataset.mode='live';
      txEl.dataset.content=d.transcript;
      txEl.textContent=d.transcript;
    }
    bLive.style.display='flex';
    bFinal.style.display='none';
    bTrunc.style.display='none';
    document.getElementById('txPreview').textContent=d.transcript.slice(-600);
  }

  /* ── Final summary ── */
  if(d.finalSummary&&d.finalSummary.trim().length>10&&!summaryReady){
    summaryReady=true;
    var rendered=renderMd(d.finalSummary);
    document.getElementById('sumFinal').innerHTML=rendered||d.finalSummary;
    document.getElementById('dlBtn').style.display='inline-flex';
    if(d.summaryMeta){
      var sm=document.getElementById('sumMeta');
      sm.textContent=d.summaryMeta;sm.style.display='block';
    }
    /* unlock chat (sidebar + bottom nav) */
    var nb=document.getElementById('chatNavBtn');
    nb.classList.remove('locked');
    var lk=document.getElementById('chatLock');if(lk)lk.remove();
    var bni=document.getElementById('bniChat');if(bni)bni.classList.remove('locked-bnav');
    toast('Summary ready! Ask AI is now unlocked','ok');
    histLoaded=false;
    goTab('summary');
  }

  prevState=state;
}

function tickDur(){
  if(!meetingStart)return;
  var s=Math.floor((Date.now()-meetingStart)/1000);
  var m=Math.floor(s/60),sc=s%60;
  document.getElementById('stDur').textContent=m+':'+(sc<10?'0':'')+sc;
}
function fmtUp(s){var h=Math.floor(s/3600),m=Math.floor((s%3600)/60);return h>0?h+'h '+m+'m':m+'m '+(s%60)+'s'}

/* ── History ─────────────────────────── */
async function loadHistory(showToast){
  histLoaded=true;
  var el=document.getElementById('histList');
  el.innerHTML='<div class="hloading">Loading from SD card&#8230;</div>';
  try{
    var r=await fetch('/api/history');
    if(!r.ok) throw new Error('HTTP '+r.status);
    histItems=await r.json();
    if(!histItems||!histItems.length){
      el.innerHTML='<div class="hempty"><div class="ei"><svg style="width:32px;height:32px;vertical-align:middle;display:inline-block;flex-shrink:0" width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" xmlns="http://www.w3.org/2000/svg"><circle cx="8" cy="8" r="6"/><polyline points="8 5 8 8 10.5 10.5"/></svg></div><p>No past meetings found on the SD card.</p></div>';
      document.getElementById('histBadge').style.display='none';
      if(showToast) toast('No past meetings found','info');
      return;
    }
    var badge=document.getElementById('histBadge');
    badge.textContent=histItems.length;badge.style.display='inline';

    var html='<div class="hcount">'+histItems.length+' meeting'+(histItems.length===1?'':'s')+' stored on SD</div>';
    histItems.forEach(function(m,i){
      var dir=m.dir||'';
      var disp=dir.replace(/^meeting_/,'').replace(/_/g,' ');
      var hasSummary=m.summary&&m.summary.trim().length>10;
      var sumText=hasSummary?m.summary.trim():'No summary available for this meeting.';
      var isLong=sumText.length>200;

      html+='<div class="hc" id="hcard-'+i+'">';
      html+='<div class="hh"><div style="flex:1;min-width:0"><div class="hname">'+esc(disp)+'</div><div class="hts">'+esc(dir)+'</div></div>';
      html+='<div class="hactions">';
      if(hasSummary){
        html+='<button class="btn btn-ai btn-sm" onclick="askHistAI('+i+')" title="Ask AI about this meeting"><svg style="width:12px;height:12px;vertical-align:middle;display:inline-block;flex-shrink:0" width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" xmlns="http://www.w3.org/2000/svg"><path d="M13.5 10a1.5 1.5 0 01-1.5 1.5H5L2 14.5V3.5A1.5 1.5 0 013.5 2h9A1.5 1.5 0 0114 3.5v6.5z"/></svg> Ask AI</button>';
        html+='<button class="btn btn-s btn-sm" onclick="dlHistSum('+i+')" title="Download summary"><svg style="width:12px;height:12px;vertical-align:middle;display:inline-block;flex-shrink:0" width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" xmlns="http://www.w3.org/2000/svg"><path d="M8 2.5v8M5.5 8l2.5 2.5 2.5-2.5"/><path d="M3 13.5h10"/></svg></button>';
      }
      html+='<button class="btn btn-s btn-sm" onclick="regenMeeting('+i+')" title="Regenerate summary from transcript"><svg style="width:12px;height:12px;vertical-align:middle;display:inline-block;flex-shrink:0" width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" xmlns="http://www.w3.org/2000/svg"><path d="M2.5 8a5.5 5.5 0 019.4-3.9"/><polyline points="12 2 12 5 9 5"/><path d="M13.5 8a5.5 5.5 0 01-9.4 3.9"/><polyline points="4 14 4 11 7 11"/></svg></button>';
      html+='<button class="btn btn-d btn-sm" onclick="delMeeting('+i+')" title="Delete meeting"><svg style="width:12px;height:12px;vertical-align:middle;display:inline-block;flex-shrink:0" width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" xmlns="http://www.w3.org/2000/svg"><polyline points="2.5 4.5 13.5 4.5"/><path d="M5.5 4.5V3.5a1 1 0 011-1h3a1 1 0 011 1v1"/><path d="M6.5 7.5v4M9.5 7.5v4"/><path d="M3.5 4.5l.8 8.5a1.5 1.5 0 001.5 1.5h4.4a1.5 1.5 0 001.5-1.5l.8-8.5"/></svg></button>';
      html+='</div></div>';
      html+='<div class="hbody">';
      html+='<div class="hsum'+(isLong?' clamped':'')+'" id="hsum-'+i+'">'+renderMd(sumText)+'</div>';
      html+='<div class="hfooter" id="hfoot-'+i+'">';
      if(isLong) html+='<button class="btn btn-s btn-sm" onclick="expandHS('+i+')">Show more</button>';
      html+='</div>';
      html+='</div></div>';
    });
    el.innerHTML=html;
    if(showToast) toast('History refreshed — '+histItems.length+' meeting'+(histItems.length===1?'':'s'),'ok');
  }catch(e){
    el.innerHTML='<div class="hempty"><div class="ei"><svg style="width:32px;height:32px;vertical-align:middle;display:inline-block;flex-shrink:0" width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" xmlns="http://www.w3.org/2000/svg"><path d="M8 2L14.5 13.5H1.5L8 2z"/><line x1="8" y1="7" x2="8" y2="10"/><circle cx="8" cy="12" r=".6" fill="currentColor" stroke="none"/></svg></div><p>Failed to load history.</p></div>';
    if(showToast) toast('Failed to load history','err');
    histLoaded=false;
  }
}

function expandHS(i){
  var el=document.getElementById('hsum-'+i);if(!el)return;
  el.classList.remove('clamped');el.classList.add('expanded');
  var f=document.getElementById('hfoot-'+i);if(f) f.innerHTML='<button class="btn btn-s btn-sm" onclick="collapseHS('+i+')">Show less</button>';
}
function collapseHS(i){
  var el=document.getElementById('hsum-'+i);if(!el)return;
  el.classList.add('clamped');el.classList.remove('expanded');
  var f=document.getElementById('hfoot-'+i);if(f) f.innerHTML='<button class="btn btn-s btn-sm" onclick="expandHS('+i+')">Show more</button>';
}
function dlHistSum(i){
  if(!histItems[i]) return;
  var t=histItems[i].summary||'No summary';
  var a=document.createElement('a');
  a.href='data:text/plain;charset=utf-8,'+encodeURIComponent(t);
  a.download=(histItems[i].dir||'meeting')+'_summary.txt';
  a.click();
}

/* Ask AI about a historical meeting */
function askHistAI(i){
  if(!histItems[i]||!histItems[i].summary) return;
  var m=histItems[i];
  histCtxSummary=m.summary;
  histCtxName=(m.dir||'meeting').replace(/^meeting_/,'').replace(/_/g,' ');
  chatMode='history';
  chatHist=[];

  /* show context banner */
  document.getElementById('chatCtxName').textContent=histCtxName;
  document.getElementById('chatCtxBanner').style.display='flex';

  /* unlock and open chat (sidebar + bottom nav) */
  var nb=document.getElementById('chatNavBtn');
  nb.classList.remove('locked');
  var lk=document.getElementById('chatLock');if(lk)lk.remove();
  var bni=document.getElementById('bniChat');if(bni)bni.classList.remove('locked-bnav');

  /* reset chat messages */
  var msgs=document.getElementById('chatMsgs');
  msgs.innerHTML='<div class="cw" id="chatWelcome"><div class="cw-icon"><svg style="width:24px;height:24px;vertical-align:middle;display:inline-block;flex-shrink:0" width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" xmlns="http://www.w3.org/2000/svg"><circle cx="8" cy="8" r="6"/><polyline points="8 5 8 8 10.5 10.5"/></svg></div><div class="cw-title">Asking about a past meeting</div><div class="cw-sub">'+esc(histCtxName)+'<br><br>Ask anything about what was discussed in this meeting.</div><div class="sug-grid"><div class="sug" onclick="askSug(this)">What were the key decisions?</div><div class="sug" onclick="askSug(this)">List all action items</div><div class="sug" onclick="askSug(this)">Who was responsible for what?</div><div class="sug" onclick="askSug(this)">Summarise in 3 bullets</div></div></div>';

  goTab('chat');
}

function clearHistCtx(){
  chatMode='current';histCtxSummary='';histCtxName='';chatHist=[];
  document.getElementById('chatCtxBanner').style.display='none';
  if(!summaryReady){
    var nb=document.getElementById('chatNavBtn');
    nb.classList.add('locked');
    if(!document.getElementById('chatLock')){
      var lk=document.createElement('span');lk.id='chatLock';
      lk.style.cssText='margin-left:auto;font-size:10px;opacity:.5';lk.innerHTML='<svg style="width:11px;height:11px;vertical-align:middle;display:inline-block;flex-shrink:0" width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" xmlns="http://www.w3.org/2000/svg"><rect x="3.5" y="7" width="9" height="7.5" rx="1.5"/><path d="M5.5 7V5.5a2.5 2.5 0 015 0V7"/></svg>';
      nb.appendChild(lk);
    }
    var bni=document.getElementById('bniChat');if(bni)bni.classList.add('locked-bnav');
    goTab('dashboard');
    toast('No current meeting summary — complete a meeting first','info');
  } else {
    toast('Switched to current meeting context','ok');
  }
}

/* ── Factory Reset ─────────────────────── */
async function factoryReset(){
  if(!confirm('⚠️ FACTORY RESET\n\nThis will permanently delete:\n• All meetings (transcripts, audio, summaries)\n• WiFi credentials\n• API keys (ElevenLabs, OpenAI)\n• Hotspot (AP) settings\n\nThe device will reboot into setup mode.\n\nContinue?')) return;

  // Second confirmation — type to confirm
  var typed=prompt('Type RESET (in capitals) to confirm factory reset:');
  if(typed!=='RESET'){
    toast('Factory reset cancelled','info');
    return;
  }

  toast('Wiping device&#8230;','info');
  try{
    var r=await fetch('/api/factory-reset',{method:'POST'});
    var data;
    try{data=await r.json()}catch(je){data={ok:r.ok}}
    if(data.ok){
      document.body.innerHTML='<div style="display:flex;flex-direction:column;align-items:center;justify-content:center;min-height:100vh;font-family:Inter,system-ui,sans-serif;color:#cdd6f4;background:#11111b;text-align:center;padding:20px"><div style="font-size:48px;margin-bottom:20px">↺</div><div style="font-size:22px;font-weight:600;margin-bottom:12px">Device Reset Complete</div><div style="font-size:14px;color:#9399b2;max-width:400px;line-height:1.6">The device is rebooting and will come up in setup mode.<br><br>Look for the WiFi network <strong style="color:#89b4fa">MeetingRecorder</strong> on your phone, then open <strong style="color:#89b4fa">http://192.168.4.1/setup</strong> to reconfigure.</div></div>';
    } else {
      toast('Factory reset failed: '+(data.error||'server error'),'err');
    }
  }catch(e){
    // After reboot, fetch will fail — that's actually the success signal.
    document.body.innerHTML='<div style="display:flex;flex-direction:column;align-items:center;justify-content:center;min-height:100vh;font-family:Inter,system-ui,sans-serif;color:#cdd6f4;background:#11111b;text-align:center;padding:20px"><div style="font-size:48px;margin-bottom:20px">↺</div><div style="font-size:22px;font-weight:600;margin-bottom:12px">Device Reset Complete</div><div style="font-size:14px;color:#9399b2;max-width:400px;line-height:1.6">The device is rebooting and will come up in setup mode.<br><br>Look for the WiFi network <strong style="color:#89b4fa">MeetingRecorder</strong> on your phone, then open <strong style="color:#89b4fa">http://192.168.4.1/setup</strong> to reconfigure.</div></div>';
  }
}

async function regenMeeting(i){
  if(!histItems[i]) return;
  var dir=histItems[i].dir;
  if(!confirm('Regenerate the summary for "'+dir+'"?\n\nThis will re-run the AI on the saved transcript and overwrite the current summary. Can take 30 sec to several minutes for long meetings.')) return;

  var card=document.getElementById('hcard-'+i);
  if(card) card.style.opacity='0.5';
  toast('Regenerating summary&#8230; may take a few minutes for long meetings','info');

  try{
    var r=await fetch('/api/history/regenerate?dir='+encodeURIComponent(dir),{
      method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded'},
      body:'dir='+encodeURIComponent(dir)
    });
    var data;
    try{data=await r.json()}catch(je){data={ok:r.ok}}
    if(data.ok){
      toast('Summary regenerated for "'+dir+'"','ok');
      histLoaded=false;
      loadHistory(false);
    } else {
      if(card) card.style.opacity='1';
      toast('Regenerate failed: '+(data.error||'server error'),'err');
    }
  }catch(e){
    if(card) card.style.opacity='1';
    toast('Connection error during regenerate','err');
  }
}

async function delMeeting(i){
  if(!histItems[i]) return;
  var dir=histItems[i].dir;
  if(!confirm('Permanently delete "'+dir+'" and all its recordings?\n\nThis cannot be undone.')) return;

  var card=document.getElementById('hcard-'+i);
  if(card) card.style.opacity='0.4';

  try{
    /* Use query param to avoid URL-encode edge cases */
    var r=await fetch('/api/history/delete?dir='+encodeURIComponent(dir),{
      method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded'},
      body:'dir='+encodeURIComponent(dir)
    });
    var data;
    try{data=await r.json()}catch(je){data={ok:r.ok}}
    if(data.ok){
      toast('"'+dir+'" deleted','ok');
      if(card){card.style.transition='height .3s,opacity .3s,padding .3s,margin .3s';card.style.overflow='hidden';card.style.height=card.offsetHeight+'px';requestAnimationFrame(function(){card.style.height='0';card.style.opacity='0';card.style.paddingTop='0';card.style.paddingBottom='0';card.style.marginBottom='0'});setTimeout(function(){histLoaded=false;loadHistory(false)},320)}
    } else {
      if(card) card.style.opacity='1';
      toast('Delete failed: '+(data.error||'server error'),'err');
    }
  }catch(e){
    if(card) card.style.opacity='1';
    toast('Connection error during delete','err');
  }
}

/* ── Chat ────────────────────────────── */
function autoH(el){el.style.height='auto';el.style.height=Math.min(el.scrollHeight,100)+'px'}
function chatKey(e){if(e.key==='Enter'&&!e.shiftKey){e.preventDefault();sendChat()}}
function askSug(el){document.getElementById('chatInp').value=el.textContent;sendChat()}

function addMsg(role,html){
  var w=document.getElementById('chatWelcome');if(w)w.remove();
  var wrap=document.getElementById('chatMsgs');
  var d=document.createElement('div');d.className='msg '+role;
  d.innerHTML='<div class="mav">'+(role==='ai'?'<svg style="width:14px;height:14px;vertical-align:middle;display:inline-block;flex-shrink:0" width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" xmlns="http://www.w3.org/2000/svg"><path d="M8 2l1.2 2.8L12 6l-2.8 1.2L8 10l-1.2-2.8L4 6l2.8-1.2L8 2z"/><path d="M12.5 10l.6 1.4 1.4.6-1.4.6-.6 1.4-.6-1.4-1.4-.6 1.4-.6.6-1.4z"/></svg>':'<svg style="width:14px;height:14px;vertical-align:middle;display:inline-block;flex-shrink:0" width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" xmlns="http://www.w3.org/2000/svg"><circle cx="8" cy="5.5" r="2.5"/><path d="M3 13.5c0-2.76 2.24-5 5-5s5 2.24 5 5"/></svg>')+'</div><div class="mbub">'+html+'</div>';
  wrap.appendChild(d);wrap.scrollTop=wrap.scrollHeight;
  return d;
}
function showThink(){
  var w=document.getElementById('chatMsgs');
  var d=document.createElement('div');d.className='msg ai';d.id='think';
  d.innerHTML='<div class="mav"><svg style="width:14px;height:14px;vertical-align:middle;display:inline-block;flex-shrink:0" width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" xmlns="http://www.w3.org/2000/svg"><path d="M8 2l1.2 2.8L12 6l-2.8 1.2L8 10l-1.2-2.8L4 6l2.8-1.2L8 2z"/><path d="M12.5 10l.6 1.4 1.4.6-1.4.6-.6 1.4-.6-1.4-1.4-.6 1.4-.6.6-1.4z"/></svg></div><div class="think"><div class="tdot"></div><div class="tdot"></div><div class="tdot"></div></div>';
  w.appendChild(d);w.scrollTop=w.scrollHeight;
}
function rmThink(){var t=document.getElementById('think');if(t)t.remove()}

async function sendChat(){
  var inp=document.getElementById('chatInp');
  var q=inp.value.trim();if(!q)return;
  var btn=document.getElementById('sendBtn');
  btn.disabled=true;inp.value='';inp.style.height='20px';
  addMsg('user',escHtml(q));showThink();
  chatHist.push({role:'user',content:q});
  try{
    var payload={question:q,history:chatHist.slice(-6)};
    /* include context override for history meetings */
    if(chatMode==='history'&&histCtxSummary) payload.context=histCtxSummary;
    var r=await fetch('/api/chat',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload)});
    var data=await r.json();
    rmThink();
    var ans=data.answer||'Sorry, no response was generated.';
    addMsg('ai',renderAns(ans));
    chatHist.push({role:'assistant',content:ans});
  }catch(e){rmThink();addMsg('ai',renderAns('[ERR] Connection error — please try again.'))}
  btn.disabled=false;
  document.getElementById('chatInp').focus();
}

/* ── Settings ───────────────────────── */
function loadCfg(){
  ['cSSID','cPass','cEL','cOAI'].forEach(function(id){document.getElementById(id).value=''});
}
async function saveCfg(){
  var p=new URLSearchParams();
  p.append('ssid',document.getElementById('cSSID').value);
  p.append('pass',document.getElementById('cPass').value);
  p.append('el_key',document.getElementById('cEL').value);
  p.append('openai_key',document.getElementById('cOAI').value);
  p.append('tz_min',document.getElementById('cTZ').value);
  try{
    var r=await fetch('/api/config',{method:'POST',body:p});
    if(!r.ok){toast('Save failed — check connection','err');return;}
    toast('Settings saved &check;','ok');
  }catch(e){toast('Error saving settings','err')}
}

/* ── Copy / Download ────────────────── */
/* navigator.clipboard requires HTTPS or localhost. The device serves HTTP,
   so modern browsers reject it. Fall back to a hidden-textarea + execCommand. */
function safeCopy(text,label){
  if(window.isSecureContext && navigator.clipboard && navigator.clipboard.writeText){
    navigator.clipboard.writeText(text).then(
      function(){toast(label+' copied','ok')},
      function(){legacyCopy(text,label)}
    );
  } else {
    legacyCopy(text,label);
  }
}
function legacyCopy(text,label){
  var ta=document.createElement('textarea');
  ta.value=text;
  ta.setAttribute('readonly','');
  ta.style.cssText='position:fixed;top:0;left:0;opacity:0;pointer-events:none';
  document.body.appendChild(ta);
  ta.focus();ta.select();ta.setSelectionRange(0,ta.value.length);
  var ok=false;
  try{ok=document.execCommand('copy')}catch(e){}
  document.body.removeChild(ta);
  toast(ok?(label+' copied'):'Copy failed — select & copy manually',ok?'ok':'err');
}
function copyTx(){
  safeCopy(document.getElementById('txFull').textContent,'Transcript');
}
function copySum(){
  var el=document.getElementById('sumFinal');
  safeCopy(el.innerText||el.textContent,'Summary');
}
function dlSummary(){
  var el=document.getElementById('sumFinal');
  var t=el.innerText||el.textContent;
  var a=document.createElement('a');
  a.href='data:text/plain;charset=utf-8,'+encodeURIComponent(t);
  a.download='meeting_summary.txt';a.click();
}

/* ── Init ───────────────────────────── */
fetch('/api/settime',{method:'POST',headers:{'Content-Type':'application/json'},
  body:JSON.stringify({ts:Date.now()})}).catch(function(){});
poll();
setInterval(poll,2500);
</script>
</body>
</html>
)rawhtml";

// ═══════════════════════════════════════════════════════════════════
//  SETUP PAGE — redirect to settings tab
// ═══════════════════════════════════════════════════════════════════
const char SETUP_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html>
<head><meta http-equiv="refresh" content="0;url=/#settings"><title>Redirecting&#8230;</title></head>
<body style="background:#0a0a0f;color:#9898aa;font-family:sans-serif;display:flex;align-items:center;justify-content:center;height:100dvh">
  <p>Redirecting to settings&#8230;</p>
</body>
</html>
)rawhtml";