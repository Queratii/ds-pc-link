/*
 * page.h - interface web embarquee (stockee en flash, PROGMEM).
 */
#pragma once

#include <Arduino.h>

static const char PAGE_INDEX[] PROGMEM = R"HTML(<!doctype html>
<html lang="fr"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP-Pont</title>
<style>
:root{
--bg:#0e1013;--pan:#15181d;--pan2:#1a1e24;--line:#242931;--line2:#2f3640;
--tx:#dde2e8;--mut:#7f8894;--acc:#d08a2c;--ok:#57a06f;--warn:#c9902f;--err:#bf5a5a;
--mono:ui-monospace,SFMono-Regular,"Cascadia Mono",Menlo,Consolas,monospace;
}
*{box-sizing:border-box}
html,body{margin:0}
body{background:var(--bg);color:var(--tx);-webkit-font-smoothing:antialiased;
font:14px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Helvetica,Arial,sans-serif}
header{display:flex;align-items:center;gap:11px;padding:0 16px;height:52px;
background:var(--pan);border-bottom:1px solid var(--line);position:sticky;top:0;z-index:5}
.brand{font-size:15px;font-weight:600;letter-spacing:.15em}
.brand i{color:var(--mut);font-weight:400;font-style:normal}
.ver{font:11px/1 var(--mono);color:var(--mut);border:1px solid var(--line2);padding:3px 5px;border-radius:2px}
.sp{flex:1}
.state{display:flex;align-items:center;gap:7px;font-size:12.5px;color:var(--mut)}
.dot{width:8px;height:8px;border-radius:50%;background:var(--mut);flex:none}
.dot.ok{background:var(--ok);box-shadow:0 0 0 3px rgba(87,160,111,.14)}
.dot.warn{background:var(--warn);box-shadow:0 0 0 3px rgba(201,144,47,.14)}
.dot.err{background:var(--err);box-shadow:0 0 0 3px rgba(191,90,90,.14)}
.wrap{max-width:720px;margin:0 auto;padding:16px}
nav{display:flex;gap:2px;border-bottom:1px solid var(--line);margin-bottom:18px;overflow-x:auto}
nav button{background:none;border:0;border-bottom:2px solid transparent;color:var(--mut);
padding:10px 15px;font-size:13.5px;cursor:pointer;white-space:nowrap;font-family:inherit}
nav button:hover{color:var(--tx)}
nav button.on{color:var(--tx);border-bottom-color:var(--acc)}
section{display:none}
section.on{display:block}
.pan{border:1px solid var(--line);background:var(--pan);border-radius:3px;margin-bottom:14px}
.ph{display:flex;align-items:center;gap:10px;padding:10px 14px;border-bottom:1px solid var(--line);
font-size:11px;letter-spacing:.13em;text-transform:uppercase;color:var(--mut)}
.pb{padding:14px}
.rows{display:grid;grid-template-columns:repeat(auto-fit,minmax(218px,1fr))}
.r{display:flex;justify-content:space-between;align-items:baseline;gap:12px;
padding:7px 14px;border-bottom:1px solid var(--line);font-size:13px}
.r b{font-weight:400;color:var(--mut);white-space:nowrap}
.r span{font-family:var(--mono);font-size:12.5px;text-align:right;word-break:break-all}
label{display:block;font-size:11px;text-transform:uppercase;letter-spacing:.1em;color:var(--mut);margin:16px 0 6px}
.pb>label:first-child{margin-top:0}
input[type=text],input[type=password],input[type=number],select{width:100%;background:#0b0d10;
border:1px solid var(--line2);color:var(--tx);border-radius:2px;padding:9px 10px;font-size:14px;font-family:var(--mono)}
select{font-family:inherit}
input:focus,select:focus{outline:none;border-color:var(--acc)}
input[readonly]{color:var(--tx);background:#101318}
input[type=range]{width:100%;accent-color:var(--acc);padding:0}
.duo{display:flex;gap:12px;flex-wrap:wrap}
.duo>div{flex:1;min-width:140px}
.chk{display:flex;align-items:center;gap:9px;margin:15px 0;font-size:13.5px}
.chk input{width:16px;height:16px;accent-color:var(--acc);flex:none}
.chk label{margin:0;text-transform:none;letter-spacing:0;font-size:13.5px;color:var(--tx)}
.note{font-size:12.5px;color:var(--mut);border-left:2px solid var(--line2);padding:1px 0 1px 11px;margin:14px 0}
.note.w{border-left-color:var(--warn)}
.hint{font-size:12px;color:var(--mut);margin-top:7px}
.btn{background:var(--pan2);border:1px solid var(--line2);color:var(--tx);padding:9px 15px;
border-radius:2px;font-size:13.5px;cursor:pointer;font-family:inherit}
.btn:hover{border-color:var(--mut)}
.btn.pri{background:var(--acc);border-color:var(--acc);color:#191205;font-weight:600}
.btn.dan{color:var(--err);border-color:rgba(191,90,90,.38)}
.mini{margin-left:auto;background:none;border:1px solid var(--line2);color:var(--mut);
font-size:11px;padding:4px 9px;border-radius:2px;cursor:pointer;letter-spacing:0;
text-transform:none;font-family:inherit}
.mini:hover{color:var(--tx);border-color:var(--mut)}
.lnk{background:none;border:0;color:var(--acc);font-size:12.5px;cursor:pointer;padding:0;font-family:inherit}
.lnk:hover{text-decoration:underline}
.nets{max-height:330px;overflow-y:auto}
.net{display:flex;align-items:center;gap:12px;padding:10px 14px;cursor:pointer;
border-bottom:1px solid var(--line);border-left:2px solid transparent}
.net:hover{background:var(--pan2)}
.net.sel{border-left-color:var(--acc);background:var(--pan2)}
.net .nm{flex:1;font-family:var(--mono);font-size:13px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.net .mt{font-family:var(--mono);font-size:11.5px;color:var(--mut);white-space:nowrap}
.nb{color:var(--warn)}
.sig{display:flex;align-items:flex-end;gap:2px;height:14px;flex:none}
.sig i{width:3px;background:var(--line2);border-radius:1px}
.sig i:nth-child(1){height:4px}
.sig i:nth-child(2){height:7px}
.sig i:nth-child(3){height:10px}
.sig i:nth-child(4){height:14px}
.sig i.on{background:var(--ok)}
.empty{padding:16px 14px;color:var(--mut);font-size:13px}
.sh{display:flex;justify-content:space-between;align-items:baseline;gap:10px;
padding-bottom:11px;border-bottom:1px solid var(--line)}
.sn{font-family:var(--mono);font-size:14px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.sk{font-size:10.5px;color:var(--acc);text-transform:uppercase;letter-spacing:.11em;white-space:nowrap}
table{width:100%;border-collapse:collapse;font-family:var(--mono);font-size:12.5px}
td{padding:8px 14px;border-bottom:1px solid var(--line)}
td.m{color:var(--mut)}
.act{position:sticky;bottom:0;background:var(--bg);border-top:1px solid var(--line);
padding:12px 0 18px;display:flex;gap:10px;flex-wrap:wrap}
#msg{position:fixed;left:50%;bottom:22px;transform:translateX(-50%);background:var(--pan2);
border:1px solid var(--line2);border-left:2px solid var(--acc);padding:11px 15px;
font-size:13px;max-width:92%;display:none;border-radius:2px;z-index:9}
</style></head><body>

<header>
<div class="brand">ESP<i>-PONT</i></div>
<div class="ver">v1.1</div>
<div class="sp"></div>
<div class="state"><span class="dot" id="dot"></span><span id="st">chargement</span></div>
</header>

<div class="wrap">

<div class="pan"><div class="rows" id="status"></div></div>

<nav>
<button data-t="up" class="on">Liaison amont</button>
<button data-t="ap">Point d'accès</button>
<button data-t="radio">Radio</button>
<button data-t="sys">Système</button>
</nav>

<form id="f" onsubmit="return false">

<section id="up" class="on">
<div class="pan">
<div class="ph">Réseaux détectés<button type="button" class="mini" id="bScan">Analyser</button></div>
<div class="nets" id="nets"><div class="empty">Lancez une analyse pour lister les réseaux à portée.</div></div>
</div>
<div class="pan"><div class="pb">
<input type="hidden" name="sta_ssid" id="ssid">
<input type="hidden" name="sta_mode" id="staMode" value="0">
<div id="noSel" class="hint">Aucun réseau sélectionné. Choisissez-en un dans la liste ci-dessus.</div>
<div id="selBox" style="display:none">
<div class="sh"><span class="sn" id="selName"></span><span class="sk" id="selKind"></span></div>
<div id="openBox"><div class="hint">Réseau ouvert, rien à saisir. Enregistrez simplement.</div></div>
<div id="persoBox">
<label>Clé du réseau</label>
<input type="password" name="sta_pass" maxlength="64" placeholder="(inchangée)" autocomplete="off">
<div class="hint">Laissez vide pour conserver la clé déjà enregistrée.</div>
<div class="hint"><button type="button" class="lnk" id="bForceEnt">Ce réseau demande plutôt un identifiant et un mot de passe</button></div>
</div>
<div id="entBox">
<label>Identifiant</label>
<input type="text" name="eap_user" maxlength="64" placeholder="prenom.nom@univ-smb.fr" autocomplete="off">
<label>Mot de passe du compte</label>
<input type="password" name="eap_pass" maxlength="64" placeholder="(inchangé)" autocomplete="off">
<label>Identité anonyme, facultative</label>
<input type="text" name="eap_anon" maxlength="64" placeholder="anonymous@univ-smb.fr" autocomplete="off">
<div class="hint">Identité extérieure transmise en clair. Vide, votre identifiant part tel quel. Le mode EAP est négocié seul avec le serveur.</div>
<div class="note w">Le certificat du serveur n'est pas vérifié, faute d'horloge sur l'ESP. Un faux point d'accès portant ce nom capterait vos identifiants.</div>
<div class="hint"><button type="button" class="lnk" id="bForcePerso">Ce réseau utilise en fait une simple clé partagée</button></div>
</div>
<div class="note w" id="bWarn" style="display:none">Ce réseau n'annonce pas les débits 802.11b. Il faudra passer l'onglet Radio en 802.11g pour s'y associer, et la DS ne verra alors plus le pont.</div>
</div>
<div class="chk"><input type="checkbox" name="sta_dhcp" id="dhcp"><label for="dhcp">Adresse IP automatique (DHCP)</label></div>
<div id="fixed">
<div class="duo">
<div><label>Adresse IP</label><input type="text" name="sta_ip"></div>
<div><label>Passerelle</label><input type="text" name="sta_gw"></div>
</div>
<div class="duo">
<div><label>Masque</label><input type="text" name="sta_mask"></div>
<div><label>DNS</label><input type="text" name="sta_dns"></div>
</div>
</div>
</div></div>
</section>

<section id="ap">
<div class="pan">
<div class="ph">Réseau diffusé par le pont</div>
<div class="pb">
<label>SSID</label>
<input type="text" name="ap_ssid" maxlength="32" autocomplete="off">
<label>Chiffrement</label>
<select name="ap_sec">
<option value="0">Ouvert — aucun chiffrement</option>
<option value="1">WPA-PSK — TKIP</option>
<option value="2">WPA2-PSK — AES</option>
<option value="3">WPA / WPA2 — mixte</option>
</select>
<div id="apPassBox">
<label>Clé (8 à 63 caractères)</label>
<input type="password" name="ap_pass" maxlength="63" placeholder="(inchangée)" autocomplete="off">
</div>
<div class="note w">Le WEP est absent car le SDK de l'ESP8266 ne sait pas créer de point d'accès WEP. Une DS ou DS Lite en mode jeu DS n'accepte que WEP ou Ouvert : choisissez <b>Ouvert</b>. Une DSi, une 3DS ou un PC acceptent WPA2.</div>
<div class="duo">
<div><label>Canal</label><select name="ap_channel"></select></div>
<div><label>Clients maximum</label><input type="number" name="ap_max" min="1" max="8"></div>
</div>
<div class="hint">Lorsque la liaison amont est établie, le canal suit celui du routeur. L'ESP8266 n'a qu'une seule radio.</div>
<div class="chk"><input type="checkbox" name="ap_hidden" id="hid"><label for="hid">Masquer le SSID</label></div>
<div class="duo">
<div><label>Adresse du pont</label><input type="text" name="ap_ip"></div>
<div><label>Masque</label><input type="text" name="ap_mask"></div>
</div>
<div class="hint">Le sous-réseau doit différer de celui du routeur.</div>
</div></div>
</section>

<section id="radio">
<div class="pan">
<div class="ph">Couche physique</div>
<div class="pb">
<label>Protocole du point d'accès</label>
<select name="phy">
<option value="1">802.11b — 11 Mbit/s, compatibilité maximale</option>
<option value="2">802.11g — 54 Mbit/s</option>
<option value="3">802.11n — débit maximal</option>
</select>
<div id="phyStaBox" style="display:none">
<label>Protocole de la liaison amont</label>
<select name="phy_sta">
<option value="1">802.11b — 11 Mbit/s</option>
<option value="2">802.11g — 54 Mbit/s</option>
<option value="3">802.11n — débit maximal</option>
</select>
</div>
<div class="note" id="phyNote">La DS ne connaît que le 802.11b, c'est pourquoi ce mode est actif par défaut sur le point d'accès.</div>
<label>Puissance d'émission — <span id="txv"></span> dBm</label>
<input type="range" name="tx_qdbm" min="0" max="82" step="1">
<div class="hint">Réduire la puissance limite la portée, utile pour un usage sur un bureau.</div>
</div></div>
</section>

<section id="sys">
<div class="pan">
<div class="ph">Routage</div>
<div class="pb">
<div class="chk"><input type="checkbox" name="nat_on" id="nat"><label for="nat">Router le trafic vers le réseau amont (NAT)</label></div>
<div class="hint">Désactivé, le point d'accès reste isolé : les clients ne voient que l'ESP.</div>
<div class="chk"><input type="checkbox" name="dns_custom_on" id="dnsc"><label for="dnsc">Imposer un serveur DNS aux clients</label></div>
<input type="text" name="dns_custom" placeholder="1.1.1.1">
<div class="hint">Sinon les clients reçoivent le DNS du routeur.</div>
</div></div>
<div class="pan">
<div class="ph">Relais dslink</div>
<div class="pb">
<div class="chk"><input type="checkbox" name="relay_on" id="rly"><label for="rly">Relayer dslink entre les DS et le PC (UDP 47474)</label></div>
<div class="hint">Les DS en mode réseau envoient leurs messages au pont, qui les transmet au PC inscrit. Côté PC : <code>dslink.py demo --relay esp-pont.local</code>, ou l'adresse amont du pont.</div>
</div></div>
<div class="pan">
<div class="ph">Administration</div>
<div class="pb">
<label>Nom d'hôte (mDNS)</label>
<input type="text" name="hostname" maxlength="24">
<label>Utilisateur</label>
<input type="text" name="admin_user" maxlength="16" autocomplete="off">
<label>Mot de passe de cette interface</label>
<input type="password" name="admin_pass" maxlength="32" placeholder="(inchangé)" autocomplete="off">
<div class="hint">Vide, l'interface n'est pas protégée. Saisissez un simple espace pour retirer un mot de passe existant.</div>
</div></div>
<div class="pan">
<div class="ph">Clients associés</div>
<table id="cli"><tr><td class="m">aucun</td></tr></table>
</div>
<div class="pan"><div class="pb">
<div style="display:flex;gap:10px;flex-wrap:wrap">
<button type="button" class="btn" onclick="post('/api/reboot','Redémarrage en cours.')">Redémarrer</button>
<button type="button" class="btn dan" id="bFact">Réinitialiser</button>
</div>
<div class="hint">Bouton FLASH de la carte : 3 s pour le point d'accès de secours, 10 s pour tout effacer.</div>
</div></div>
</section>

</form>

<div class="act"><button type="button" class="btn pri" id="bSave">Enregistrer et appliquer</button></div>
</div>
<div id="msg"></div>

<script>
var Q=function(s){return document.querySelector(s)};
var SEC=['Ouvert','WEP','WPA','WPA2','WPA/WPA2','?','WPA3'];
var scanTimer=null, scanned=false, selOpen=false;

function toast(t,ms){var m=Q('#msg');m.textContent=t;m.style.display='block';
clearTimeout(m._t);m._t=setTimeout(function(){m.style.display='none'},ms||3500)}

var ch=Q('[name=ap_channel]');
for(var i=1;i<=13;i++){var o=document.createElement('option');o.value=i;o.textContent='Canal '+i;ch.appendChild(o)}

document.querySelectorAll('nav button').forEach(function(b){b.onclick=function(){
document.querySelectorAll('nav button').forEach(function(x){x.classList.remove('on')});
document.querySelectorAll('section').forEach(function(x){x.classList.remove('on')});
b.classList.add('on');Q('#'+b.dataset.t).classList.add('on')}});

Q('#dhcp').onchange=function(){Q('#fixed').style.display=this.checked?'none':'block'};

var apSec=Q('[name=ap_sec]');
apSec.onchange=function(){Q('#apPassBox').style.display=this.value==='0'?'none':'block'};

function showBoxes(ent,sec){
selOpen=(!ent&&sec===0);
Q('#openBox').style.display=(!ent&&sec===0)?'block':'none';
Q('#persoBox').style.display=(!ent&&sec!==0)?'block':'none';
Q('#entBox').style.display=ent?'block':'none';
Q('#selKind').textContent=ent?'WPA2-Entreprise 802.1X':(sec===0?'Réseau ouvert':'Clé partagée '+SEC[sec]);
Q('#staMode').value=ent?'1':'0'}

/* Selection d'un reseau : le type d'authentification est deduit du balayage. */
function selectNet(ssid,sec,ent,b){
Q('#ssid').value=ssid;
Q('#noSel').style.display='none';
Q('#selBox').style.display='block';
Q('#selName').textContent=ssid;
Q('#bWarn').style.display=b?'none':'block';
showBoxes(ent,sec)}

Q('#bForceEnt').onclick=function(){showBoxes(true,3)};
Q('#bForcePerso').onclick=function(){showBoxes(false,3)};

function fill(o){for(var k in o){var e=document.querySelector('[name="'+k+'"]');if(!e)continue;
if(e.type==='checkbox')e.checked=!!o[k];else e.value=o[k]}}

function loadCfg(){fetch('/api/config').then(function(r){return r.json()}).then(function(o){
fill(o);Q('#dhcp').onchange();apSec.onchange();
Q('#txv').textContent=(o.tx_qdbm/4).toFixed(2);
if(o.sta_ssid){Q('#noSel').style.display='none';Q('#selBox').style.display='block';
Q('#selName').textContent=o.sta_ssid;showBoxes(o.sta_mode==1,3)}
else if(!scanned){scanned=true;startScan()}}).catch(function(){})}

function bars(rssi){var n=rssi>=-55?4:rssi>=-67?3:rssi>=-76?2:1;var s='';
for(var i=1;i<=4;i++)s+='<i class="'+(i<=n?'on':'')+'"></i>';return '<div class="sig">'+s+'</div>'}

function esc(s){return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/"/g,'&quot;')}

function startScan(){Q('#nets').innerHTML='<div class="empty">Analyse en cours…</div>';
Q('#bScan').disabled=true;poll()}

function poll(){fetch('/api/scan').then(function(r){return r.json()}).then(function(o){
if(o.scanning){clearTimeout(scanTimer);scanTimer=setTimeout(poll,1500);return}
Q('#bScan').disabled=false;
var seen={},list=[];
o.nets.sort(function(a,b){return b.rssi-a.rssi});
o.nets.forEach(function(n){if(!n.ssid)return;if(seen[n.ssid])return;seen[n.ssid]=1;list.push(n)});
if(!list.length){Q('#nets').innerHTML='<div class="empty">Aucun réseau détecté.</div>';return}
var cur=Q('#ssid').value,h='';
list.forEach(function(n){
h+='<div class="net'+(n.ssid===cur?' sel':'')+'" data-s="'+esc(n.ssid)+'" data-k="'+n.sec
+'" data-e="'+n.ent+'" data-b="'+n.b+'">'
+bars(n.rssi)+'<div class="nm">'+esc(n.ssid)+'</div>'
+'<div class="mt">'+(n.ent?'802.1X':SEC[n.sec])+' · c'+n.ch+' · '+n.rssi+' dBm'
+(n.b?'':' · <span class="nb">sans b</span>')+'</div></div>'});
Q('#nets').innerHTML=h;
Q('#nets').querySelectorAll('.net').forEach(function(el){
el.onclick=function(){
Q('#nets').querySelectorAll('.net').forEach(function(x){x.classList.remove('sel')});
el.classList.add('sel');
selectNet(el.dataset.s,parseInt(el.dataset.k,10),el.dataset.e==='1',el.dataset.b==='1')}});
}).catch(function(){Q('#bScan').disabled=false;
Q('#nets').innerHTML='<div class="empty">Analyse interrompue, réessayez.</div>'})}

Q('#bScan').onclick=function(){scanned=true;startScan()};

function kv(k,v){return '<div class="r"><b>'+k+'</b><span>'+esc(v)+'</span></div>'}

function loadStatus(){fetch('/api/status').then(function(r){return r.json()}).then(function(s){
var cls=s.link==='connecte'?'ok':(s.link==='secours'?'warn':'err');
Q('#dot').className='dot '+cls;
Q('#st').textContent=s.link==='connecte'?'liaison établie':(s.link==='secours'?'mode secours':
(s.link==='connexion'?'connexion en cours':'liaison non configurée'));
var g='';
g+=kv('Réseau amont',s.sta_ssid||'—');
g+=kv('Authentification',s.sta_auth);
g+=kv('Adresse amont',s.sta_ip);
g+=kv('Signal amont',s.rssi?s.rssi+' dBm':'—');
g+=kv('SSID du pont',s.ap_ssid);
g+=kv('Chiffrement',s.ap_sec);
g+=kv('Adresse du pont',s.ap_ip);
g+=kv('Canal',s.channel);
if(s.per_iface){g+=kv('Protocole du pont',s.phy);g+=kv('Protocole amont',s.phy_sta)}
else g+=kv('Protocole',s.phy);
Q('#phyStaBox').style.display=s.per_iface?'block':'none';
Q('#phyNote').textContent=s.per_iface
?'Cette puce accepte un protocole par interface. Le point d\'accès peut rester en 802.11b pour la console pendant que la liaison amont profite du 802.11n.'
:'Le réglage s\'applique à toute la radio, liaison amont comprise. Passez en g ou en n si aucune console n\'est concernée, ou si votre routeur refuse une association en b.';
g+=kv('Routage NAT',s.nat?'actif':'inactif');
g+=kv('Portail captif',s.portal?'actif':'inactif');
g+=kv('Clients associés',s.clients);
g+=kv('DNS annoncé',s.client_dns);
g+=kv('Relais dslink',!s.relay?'inactif':(s.relay_pc?'PC '+s.relay_pc:'aucun PC inscrit')+' · '+s.relay_ds+' DS');
g+=kv('Mémoire libre',s.heap+' o');
g+=kv('Durée de marche',s.uptime);
Q('#status').innerHTML=g;
var t='';
if(s.stations&&s.stations.length){s.stations.forEach(function(c){
t+='<tr><td>'+esc(c.ip)+'</td><td class="m">'+esc(c.mac)+'</td></tr>'})}
else t='<tr><td class="m">aucun</td></tr>';
Q('#cli').innerHTML=t}).catch(function(){})}

Q('[name=tx_qdbm]').oninput=function(){Q('#txv').textContent=(this.value/4).toFixed(2)};

function collect(){var p=new URLSearchParams();
document.querySelectorAll('#f [name]').forEach(function(e){
if(e.type==='checkbox')p.set(e.name,e.checked?'1':'0');
else if(e.type==='password'){if(e.value!=='')p.set(e.name,e.value)}
else p.set(e.name,e.value)});
/* Reseau ouvert choisi : la cle d'un ancien reseau ne doit pas etre reutilisee,
 * sinon l'ESP exigerait du WPA et l'association echouerait. */
if(selOpen)p.set('sta_pass','');
return p}

Q('#bSave').onclick=function(){
fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
body:collect().toString()}).then(function(r){return r.json()}).then(function(o){
if(o.ok){toast('Configuration appliquée. La radio redémarre, reconnectez-vous si le réseau a changé.',7000);
document.querySelectorAll('input[type=password]').forEach(function(e){e.value=''});
setTimeout(loadCfg,2500)}
else toast('Refusé : '+(o.error||'valeur invalide'),6000)})
.catch(function(){toast('Envoi effectué. Le point d\'accès a peut-être changé de nom.',7000)})};

function post(u,m){fetch(u,{method:'POST'}).then(function(){toast(m,6000)}).catch(function(){toast(m,6000)})}
Q('#bFact').onclick=function(){if(confirm('Effacer toute la configuration et redémarrer ?'))
post('/api/factory','Réinitialisation puis redémarrage.')};

loadCfg();loadStatus();setInterval(loadStatus,4000);
</script></body></html>
)HTML";
