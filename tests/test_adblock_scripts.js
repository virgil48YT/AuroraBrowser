// Offline Chromium regressions. Each case gets a fresh realm, real DOM/CSSOM,
// deterministic clock, and mocked network/player APIs. No YouTube connection
// or media download is made. Production sources come from the C++ builders.
async function auroraRunAdblockTests(fixtures){
  const results=[];
  function check(value,message){if(!value)throw new Error(message);}
  async function realm(html=""){
    const frame=document.createElement("iframe");
    frame.srcdoc="<!doctype html><html><head></head><body>"+html+"</body></html>";
    const loaded=new Promise(resolve=>{frame.onload=resolve;});
    document.body.appendChild(frame);
    await loaded;
    const w=frame.contentWindow,d=w.document;
    let time=0,sequence=0;
    const pending=new Map(),observations=[];
    const metrics={queries:0,callbacks:0};
    w.setTimeout=(fn,delay=0)=>{const id=++sequence;pending.set(id,{at:time+delay,fn});return id;};
    w.clearTimeout=id=>pending.delete(id);
    Object.defineProperty(w.performance,"now",{value:()=>time});
    const NativeObserver=w.MutationObserver;
    w.MutationObserver=class {
      constructor(fn){this.native=new NativeObserver(records=>{metrics.callbacks++;fn(records);});}
      observe(target,options){
        observations.push({document:target===d,target:target.id||target.nodeName,options});
        this.native.observe(target,options);
      }
      disconnect(){this.native.disconnect();}
    };
    for(const proto of [w.Element.prototype,w.Document.prototype]){
      const raw=proto.querySelectorAll;
      proto.querySelectorAll=function(selector){metrics.queries++;return raw.call(this,selector);};
    }
    const clock={
      get now(){return time;},
      get timers(){return Array.from(pending.values()).map(t=>t.at-time);},
      async settle(){await Promise.resolve();await Promise.resolve();},
      async advance(ms){
        await this.settle();
        const end=time+ms;
        let iterations=0;
        for(;;){
          const next=Array.from(pending).sort((a,b)=>a[1].at-b[1].at)[0];
          if(!next||next[1].at>end)break;
          if(++iterations>1000)throw new Error("runaway timer/observer loop");
          time=next[1].at;pending.delete(next[0]);next[1].fn();
          await this.settle();
        }
        time=end;await this.settle();
      }
    };
    const location={href:"https://www.youtube.com/watch?v=AAAAAAAAAAA"};
    const calls={fetch:[],open:[],seeks:[],reloads:[],clicks:0};
    let body='{"adSlots":[1],"videoDetails":{"videoId":"AAAAAAAAAAA"},"label":"adSlots"}';
    w.fetch=function(input){
      calls.fetch.push(input);
      return Promise.resolve(new w.Response(body,{status:201,headers:{"x-fixture":"yes"}}));
    };
    class FakeXHR{
      constructor(){this.readyState=0;this.responseType="";this.value="";}
      open(method,url){calls.open.push([method,url]);this.readyState=1;this.value="";}
      get responseText(){
        if(this.responseType&&this.responseType!=="text")
          throw new w.DOMException("Invalid response type","InvalidStateError");
        return this.value;
      }
      get response(){return this.value;}
    }
    w.XMLHttpRequest=FakeXHR;
    const api={
      w,d,frame,clock,metrics,observations,location,calls,
      set body(value){body=value;},
      run(script){w.Function("location",script).call(w,location);},
      async cosmetics(resources,builtin=[],youtube=false){
        if(!w.__auroraCosmeticInstalled)this.run(fixtures.cosmetic);
        w.__auroraCosmeticInstalled.update(w.JSON.stringify(resources),builtin,youtube);
        await clock.advance(60);
      },
      player(ad=false){
        d.body.innerHTML='<main><div id="movie_player" class="html5-video-player">'+
          '<div class="html5-video-container"><video class="html5-main-video"></video></div>'+
          '<div class="video-ads"></div></div></main>';
        const p=d.getElementById("movie_player"),v=p.querySelector("video");
        if(ad)p.classList.add("ad-showing");
        Object.defineProperties(v,{
          paused:{configurable:true,writable:true,value:false},
          seeking:{configurable:true,writable:true,value:false},
          duration:{configurable:true,writable:true,value:900},
          currentTime:{configurable:true,writable:true,value:42}
        });
        p.getVideoData=()=>({video_id:"AAAAAAAAAAA"});
        p.getProgressState=()=>({duration:15});
        p.getStatsForNerds=()=>({debug_info:""});
        p.seekTo=(end,allow)=>calls.seeks.push({at:clock.now,end,allow});
        p.loadVideoById=(id,start)=>calls.reloads.push({at:clock.now,id,start});
        return {p,v};
      }
    };
    return api;
  }
  async function test(name,fn){
    let r;
    try{r=await realm();await fn(r);results.push({name,passed:true});}
    catch(error){results.push({name,passed:false,error:String(error.stack||error)});}
    finally{if(r)r.frame.remove();}
  }

  await test("cosmetics: styles, exceptions, attributes and classes",async r=>{
    r.d.body.innerHTML='<div class="ad"></div><div class="excepted"></div>'+
      '<div class="styled"></div><div class="attrs sponsored promoted" data-ad="1" title="ad"></div>';
    await r.cosmetics({
      hide_selectors:[".ad",".excepted","invalid["],
      exceptions:[".excepted"],
      non_hide_styles:{".styled":["color:rgb(1, 2, 3)!important","border-top-width:7px"]},
      remove_attrs:{".attrs":["data-ad","title"]},
      remove_classes:{".attrs":["sponsored","promoted"]}
    },[".excepted"]);
    check(r.w.getComputedStyle(r.d.querySelector(".ad")).display==="none","ad not hidden");
    check(r.w.getComputedStyle(r.d.querySelector(".excepted")).display!=="none","exception hidden");
    check(r.w.getComputedStyle(r.d.querySelector(".styled")).color==="rgb(1, 2, 3)","style missing");
    const attrs=r.d.querySelector(".attrs");
    check(!attrs.hasAttribute("title")&&!attrs.hasAttribute("data-ad"),"attributes not removed");
    check(!attrs.classList.contains("sponsored")&&!attrs.classList.contains("promoted"),"classes not removed");
    attrs.setAttribute("data-ad","again");attrs.classList.add("sponsored");
    await r.clock.advance(100);
    check(!attrs.hasAttribute("data-ad")&&!attrs.classList.contains("sponsored"),"reapplied actions not removed");
  });
  await test("cosmetics: encoded CSS-only actions and unsupported predicates",async r=>{
    r.d.body.innerHTML='<div class="style-action"></div><div class="attr-action" data-ad="1"></div>'+
      '<div class="class-action sponsored"></div><div class="remove-action"></div>'+
      '<div class="unsupported"></div>';
    await r.cosmetics({procedural_actions:[
      JSON.stringify({selector:[{type:"css-selector",arg:".style-action"}],action:{type:"style",arg:"color:rgb(4,5,6)"}}),
      JSON.stringify({selector:[{CssSelector:".attr-action"}],action:{RemoveAttr:"data-ad"}}),
      JSON.stringify({selector:".class-action",action:{type:"remove-class",arg:"sponsored"}}),
      JSON.stringify({selector:".remove-action",action:"remove"}),
      JSON.stringify({selector:[{type:"css-selector",arg:".unsupported"},{type:"has-text",arg:"ad"}],action:"remove"}),
      "invalid JSON"
    ]});
    check(r.w.getComputedStyle(r.d.querySelector(".style-action")).color==="rgb(4, 5, 6)","encoded style missing");
    check(!r.d.querySelector(".attr-action").hasAttribute("data-ad"),"encoded attribute action missing");
    check(!r.d.querySelector(".class-action").classList.contains("sponsored"),"encoded class action missing");
    check(!r.d.querySelector(".remove-action"),"encoded removal missing");
    check(r.d.querySelector(".unsupported"),"unsupported predicate broadened into a destructive selector");
  });
  await test("cosmetics: pipe-separated action names and C++ string quoting",async r=>{
    const escaped=r.d.createElement("div");
    escaped.setAttribute("data-escape",'quote" and slash\\ and 雪');
    r.d.body.appendChild(escaped);
    r.run(fixtures.cosmetic);await r.clock.advance(60);
    check(r.w.getComputedStyle(escaped).display==="none","C++ quoting changed a selector");
    r.d.body.insertAdjacentHTML("beforeend",'<div class="target sponsor promoted" data-ad="1" title="ad"></div>');
    await r.cosmetics({
      remove_attrs:{".target":"data-ad|title"},
      remove_classes:{".target":"sponsor|promoted"}
    });
    const target=r.d.querySelector(".target");
    check(!target.hasAttribute("data-ad")&&!target.hasAttribute("title"),"pipe-separated attributes ignored");
    check(target.className==="target","pipe-separated classes ignored");
  });
  await test("cosmetics: CSS-only mutations perform no DOM selector walks",async r=>{
    await r.cosmetics({hide_selectors:[".ad"]});
    const before=r.metrics.queries;
    const parent=r.d.createElement("section");
    parent.innerHTML='<div class="ad"></div>'.repeat(300);
    r.d.body.appendChild(parent);
    await r.clock.advance(150);
    check(r.metrics.queries===before,"CSS-only observer walked nodes");
    check(r.w.getComputedStyle(parent.firstElementChild).display==="none","dynamic CSS hiding missing");
    check(!r.observations.some(x=>x.document&&x.options.attributes),"CSS-only rules observe attributes");
  });
  await test("cosmetics: bursts coalesce, descendants deduplicate and self-writes settle",async r=>{
    await r.cosmetics({remove_attrs:{".ad":["data-ad"]}});
    const before=r.metrics.queries;
    const parent=r.d.createElement("section");
    r.d.body.appendChild(parent);
    for(let i=0;i<300;i++){
      const node=r.d.createElement("div");node.className="ad";node.setAttribute("data-ad","yes");
      parent.appendChild(node);
      node.appendChild(r.d.createElement("span"));
    }
    await r.clock.advance(250);
    const queries=r.metrics.queries-before;
    check(queries<=8,"overlapping subtree walked repeatedly: "+queries);
    check(!parent.querySelector("[data-ad]"),"burst additions not processed");
    const count=r.metrics.callbacks;
    await r.clock.advance(1000);
    check(r.metrics.callbacks===count,"self-induced observer loop");
  });
  await test("cosmetics: stylesheet removal, edits and head replacement recover",async r=>{
    await r.cosmetics({hide_selectors:[".ad"]});
    r.d.getElementById("aurora-cosmetic-filters").remove();
    await r.clock.advance(80);
    check(r.d.getElementById("aurora-cosmetic-filters"),"removed stylesheet not restored");
    r.d.getElementById("aurora-cosmetic-filters").textContent="";
    await r.clock.advance(80);
    check(r.d.getElementById("aurora-cosmetic-filters").textContent,"edited stylesheet not restored");
    r.d.head.replaceWith(r.d.createElement("head"));
    await r.clock.advance(80);
    check(r.d.head.querySelector("#aurora-cosmetic-filters"),"head replacement not recovered");
  });
  await test("cosmetics: repeated install reuses observer and updates resources",async r=>{
    await r.cosmetics({hide_selectors:[".first"]});
    const guard=r.w.__auroraCosmeticInstalled,count=r.observations.length;
    await r.cosmetics({hide_selectors:[".first"]});
    check(r.observations.length===count,"identical payload restarted observer");
    r.run(fixtures.cosmetic);
    check(r.w.__auroraCosmeticInstalled===guard,"idempotency guard replaced");
    await r.cosmetics({hide_selectors:[".second"]});
    check(r.d.getElementById("aurora-cosmetic-filters").textContent.includes(".second"),"new rules not installed");
    await r.cosmetics({});
    check(!r.d.getElementById("aurora-cosmetic-filters"),"empty update left stale CSS");
  });
  await test("cosmetics: large selector lists are chunked and invalid rules isolated",async r=>{
    const selectors=Array.from({length:513},(_,i)=>".ad-"+i);
    selectors.splice(30,0,"invalid[");
    r.d.body.innerHTML='<div class="ad-0"></div><div class="ad-512"></div>';
    await r.cosmetics({hide_selectors:selectors});
    const style=r.d.getElementById("aurora-cosmetic-filters");
    check(style.tagName==="STYLE","not a real stylesheet element");
    check(style.sheet.cssRules.length>=9,"selector list not chunked");
    check(Array.from(style.sheet.cssRules).every(rule=>rule.selectorText.split(",").length<=64),
      "oversized selector batch");
    for(const node of r.d.body.children)
      check(r.w.getComputedStyle(node).display==="none","invalid selector poisoned a batch");
  });
  await test("cosmetics: exception updates and shields-down unwind inline hides",async r=>{
    r.d.body.innerHTML='<div class="builtin" style="display:block!important"></div>';
    const node=r.d.querySelector(".builtin");
    await r.cosmetics({},[".builtin"]);
    check(node.style.display==="none","builtin inline enforcement missing");
    await r.cosmetics({exceptions:[".builtin"]},[".builtin"]);
    check(node.style.display==="block","new exception left stale inline hiding");
    await r.cosmetics({},[".builtin"]);
    await r.cosmetics({});
    check(node.style.display==="block","disabled payload left stale inline hiding");
    check(!r.d.getElementById("aurora-cosmetic-filters"),"disabled payload left stylesheet");
    const count=r.metrics.callbacks;
    node.classList.add("late");await r.clock.advance(100);
    check(r.metrics.callbacks===count,"disabled payload left observer connected");
  });
  await test("cosmetics: style and removal exceptions apply before every action",async r=>{
    r.d.body.innerHTML='<div class="allowed sponsored" data-ad="1"></div>';
    const action=JSON.stringify({selector:".allowed",action:"remove"});
    await r.cosmetics({
      exceptions:[".allowed",action],
      non_hide_styles:{".allowed":"display:none!important"},
      remove_attrs:{".allowed":["data-ad"]},
      remove_classes:{".allowed":["sponsored"]},
      procedural_actions:[action]
    });
    const node=r.d.querySelector(".allowed");
    check(node&&node.hasAttribute("data-ad")&&node.classList.contains("sponsored"),"exception was mutated");
    check(r.w.getComputedStyle(node).display!=="none","style exception was hidden");
  });
  await test("cosmetics: an existing boolean installation guard is honored",async r=>{
    r.w.__auroraCosmeticInstalled=true;
    r.run(fixtures.cosmetic);
    check(r.observations.length===0,"existing guard installed another observer");
  });
  await test("cosmetics: YouTube protected nodes and ancestors survive every action",async r=>{
    const {p,v}=r.player();
    const module=p.querySelector(".video-ads");
    module.innerHTML='<div class="ytp-ad-skip-button-container">'+
      '<button class="ytp-ad-skip-button-modern"><span class="ytp-ad-skip-button-text">Skip</span></button></div>'+
      '<div class="ytp-ad-overlay-container"></div>';
    const poster=r.d.createElement("div");poster.className="ytp-cued-thumbnail-overlay";
    p.appendChild(poster);
    r.d.body.insertAdjacentHTML("beforeend",'<div class="ordinary-ad"></div>');
    for(const node of [module,v,poster,p])node.setAttribute("data-keep","yes");
    await r.cosmetics({
      hide_selectors:[".video-ads","[class*='skip-button']",".ytp-cued-thumbnail-overlay",
        "#movie_player","body",".ordinary-ad",".html5-video-container"],
      non_hide_styles:{"body":"opacity:0!important",".video-ads":"display:none!important"},
      remove_attrs:{"[data-keep]":["data-keep"],"[class*='skip-button']":["class"]},
      remove_classes:{"[class*='skip-button']":["ytp-ad-skip-button-modern"]}
    },[".ytp-ad-overlay-container",".video-ads"],true);
    for(const node of [r.d.body,p,v,module,poster,...p.querySelectorAll("[class*='skip-button']")]){
      check(r.w.getComputedStyle(node).display!=="none","protected node hidden: "+node.className);
      check(r.w.getComputedStyle(node).opacity!=="0","protected ancestor transparent");
    }
    for(const node of [module,v,poster,p])check(node.hasAttribute("data-keep"),"protected attribute removed");
    check(r.w.getComputedStyle(r.d.querySelector(".ordinary-ad")).display==="none","ordinary ad not hidden");
    check(r.w.getComputedStyle(r.d.querySelector(".ytp-ad-overlay-container")).display==="none","safe overlay not hidden");
    // Broad rules must remain safe when controls are inserted after install.
    const button=r.d.createElement("button");button.className="ytp-ad-skip-button-future";
    module.appendChild(button);await r.clock.advance(100);
    check(r.w.getComputedStyle(button).display!=="none","future skip-button variant hidden");
  });
  await test("cosmetics: protect empty YouTube player roots before children arrive",async r=>{
    r.d.body.innerHTML='<ytd-player><div id="movie_player" class="html5-video-player"></div></ytd-player>';
    await r.cosmetics({
      hide_selectors:["body","ytd-player","#movie_player"],
      remove_attrs:{"#movie_player":["id","class"]},
      procedural_actions:[JSON.stringify({selector:"ytd-player",action:"remove"})]
    },[],true);
    check(r.d.getElementById("movie_player"),"empty player lost its identity");
    check(r.d.querySelector("ytd-player"),"empty player wrapper removed");
    check(r.w.getComputedStyle(r.d.body).display!=="none","empty player page hidden");
    check(r.w.getComputedStyle(r.d.getElementById("movie_player")).display!=="none","empty player hidden");
  });
  await test("scriptlets: run once without eval, independent of cosmetic guard",async r=>{
    r.w.eval=()=>{throw new Error("page eval forbidden");};
    r.run(fixtures.scriptlet);r.run(fixtures.scriptlet);
    check(r.w.__scriptletRuns===1,"scriptlet ran more than once");
    r.run(fixtures.throwingScriptlet);
    await r.cosmetics({hide_selectors:[".ad"]});
    check(r.w.__auroraCosmeticInstalled,"throwing scriptlet broke cosmetics");
  });
  await test("PREVENT: JSON keys, value strings, deep objects, Shorts and revivers",async r=>{
    r.run(fixtures.youtube);
    const value=r.w.JSON.parse('{"adSlots":[1],"nested":{"playerAds":[2],"label":"adSlots"},"adClientParams":{"isAd":true},"number":4}',
      (key,v)=>key==="number"?v+1:v);
    check(!("adSlots" in value)&&!("playerAds" in value.nested),"ad keys survived");
    check(value.nested.label==="adSlots","string value corrupted");
    check(value.number===5,"JSON reviver changed");
    check(value.adClientParams.isAd===false,"Shorts ad flag survived");
    let deep={adPlacements:[1]};
    for(let i=0;i<40;i++)deep={child:deep};
    const clean=r.w.JSON.parse(JSON.stringify(deep));
    let node=clean;for(let i=0;i<40;i++)node=node.child;
    check(!("adPlacements" in node),"deep response was truncated");
    let threw=false;try{r.w.JSON.parse("{");}catch(e){threw=e.name==="SyntaxError";}
    check(threw,"native parse errors swallowed");
  });
  await test("PREVENT: initial global handles cycles, getters and repeated assignments",async r=>{
    r.run(fixtures.youtube);
    let getterCalls=0;
    const value={adSlots:[1],child:{playerAds:[2]}};
    value.self=value;
    Object.defineProperty(value,"getter",{enumerable:true,get(){getterCalls++;throw new Error("do not read");}});
    r.w.ytInitialPlayerResponse=value;
    check(!("adSlots" in value)&&!("playerAds" in value.child),"initial response not pruned");
    check(getterCalls===0,"page getter invoked");
    r.w.ytInitialPlayerResponse={adBreakHeartbeatParams:{}};
    check(!("adBreakHeartbeatParams" in r.w.ytInitialPlayerResponse),"setter not retained");
    const proxyValue={good:{adSlots:[1]},bad:new Proxy({},{ownKeys(){throw new Error("isolated");}})};
    r.w.ytInitialPlayerResponse=proxyValue;
    check(!Object.hasOwn(proxyValue.good,"adSlots"),"throwing object aborted its sibling's pruning");
    const parse=r.w.JSON.parse,fetch=r.w.fetch;
    r.run(fixtures.youtube);
    check(r.w.JSON.parse===parse&&r.w.fetch===fetch,"idempotency installed duplicate hooks");
  });
  await test("PREVENT: existing readonly/global accessor semantics survive",async r=>{
    const value={adSlots:[1]};
    Object.defineProperty(r.w,"ytInitialPlayerResponse",{configurable:true,get(){return value;}});
    const before=Object.getOwnPropertyDescriptor(r.w,"ytInitialPlayerResponse");
    r.run(fixtures.youtube);
    const after=Object.getOwnPropertyDescriptor(r.w,"ytInitialPlayerResponse");
    check(before.get===after.get&&!after.set,"existing accessor replaced");
    check(!("adSlots" in value),"existing initial data not pruned");
  });
  await test("PREVENT: fetch text/json/clones, URL/Request inputs and metadata",async r=>{
    r.run(fixtures.youtube);
    const response=await r.w.fetch(new r.w.URL("https://www.youtube.com/youtubei/v1/player?key=x"));
    const clone=response.clone();
    check(response.status===201&&response.headers.get("x-fixture")==="yes","response metadata changed");
    const text=await response.text();
    check(response.bodyUsed,"native bodyUsed lost");
    check(!("adSlots" in JSON.parse(text))&&JSON.parse(text).label==="adSlots","fetch text corrupted");
    const value=await clone.json();
    check(!("adSlots" in value),"clone bypassed pruning");
    const request=await r.w.fetch(new r.w.Request("https://www.youtube.com/youtubei/v1/player"));
    check(!("adSlots" in await request.json()),"Request input bypassed pruning");
    const foreign=await r.w.fetch(new URL("https://www.youtube.com/youtubei/v1/player"));
    check(!("adSlots" in await foreign.json()),"cross-realm URL input bypassed pruning");
    let consumed=false;try{await response.text();}catch(e){consumed=e instanceof r.w.TypeError;}
    check(consumed,"native consumed-body failure lost");
  });
  await test("PREVENT: unrelated/host-lookalike fetches are untouched",async r=>{
    r.run(fixtures.youtube);
    for(const url of ["https://www.youtube.com/api/stats","https://youtube.com.evil.test/youtubei/v1/player"]){
      const response=await r.w.fetch(url);
      check(Object.hasOwn(await response.json(),"adSlots"),"unrelated response was patched");
    }
  });
  await test("PREVENT: XHR completion, cached accessors, JSON and request reuse",async r=>{
    r.run(fixtures.youtube);
    const xhr=new r.w.XMLHttpRequest();
    xhr.open("POST","https://www.youtube.com/youtubei/v1/player");
    xhr.readyState=3;xhr.value='{"adSlots":[';
    check(xhr.responseText===xhr.value,"partial XHR changed");
    xhr.readyState=4;xhr.value='{"adSlots":[1],"label":"adSlots"}';
    const clean=xhr.responseText;
    check(clean===xhr.response&&clean===xhr.responseText,"XHR reads not consistent");
    check(!("adSlots" in JSON.parse(clean))&&JSON.parse(clean).label==="adSlots","XHR text not pruned");
    xhr.open("GET","https://example.test/data");
    xhr.readyState=4;xhr.value='{"adSlots":[2]}';
    check(xhr.responseText===xhr.value,"reused XHR retained player hook");
    xhr.open("GET","https://www.youtube.com/youtubei/v1/next");
    xhr.responseType="json";xhr.readyState=4;xhr.value={playerAds:[1],keep:1};
    check(xhr.response.keep===1&&!Object.hasOwn(xhr.response,"playerAds"),"XHR JSON not pruned");
    let threw=false;try{void xhr.responseText;}catch(e){threw=e.name==="InvalidStateError";}
    check(threw,"native XHR responseType error lost");
    xhr.responseType="arraybuffer";xhr.value={adSlots:[1]};
    check(Object.hasOwn(xhr.response,"adSlots"),"non-JSON XHR object changed");
  });
  await test("PREVENT: legacy form payloads and malformed bodies fail open",async r=>{
    r.body="status=ok&player_response="+encodeURIComponent('{"adPlacements":[1],"keep":true}');
    r.run(fixtures.youtube);
    const response=await r.w.fetch("https://www.youtube.com/get_video_info?video_id=AAAAAAAAAAA");
    const form=new URLSearchParams(await response.text());
    check(!Object.hasOwn(JSON.parse(form.get("player_response")),"adPlacements"),"legacy payload not pruned");
    r.body='status=ok&player_response={"adSlots":[1],"keep":true}';
    const unescaped=await r.w.fetch("https://www.youtube.com/get_video_info?video_id=AAAAAAAAAAA");
    const unescapedForm=new URLSearchParams(await unescaped.text());
    check(!Object.hasOwn(JSON.parse(unescapedForm.get("player_response")),"adSlots"),"unescaped form body not pruned");
    r.body='{"adSlots":[';
    const bad=await r.w.fetch("https://www.youtube.com/youtubei/v1/player");
    check(await bad.text()==='{"adSlots":[',"malformed body changed");
  });
  await test("DISMISS: modern nested controls click once and disabled controls are ignored",async r=>{
    const {p}=r.player(true);
    p.insertAdjacentHTML("beforeend",'<button class="ytp-ad-skip-button-modern" disabled>Disabled</button>'+
      '<button class="ytp-skip-ad-button"><span class="ytp-skip-ad-button__text">Skip</span></button>');
    const buttons=p.querySelectorAll("button");
    buttons[0].onclick=()=>{throw new Error("disabled clicked");};
    buttons[1].onclick=()=>{r.calls.clicks++;p.classList.remove("ad-showing");};
    r.run(fixtures.youtube);await r.clock.advance(500);
    check(r.calls.clicks===1,"skip button not clicked exactly once");
    check(r.calls.seeks.length===0&&r.calls.reloads.length===0,"fallback ran after native skip");
  });
  await test("DISMISS: a late skip button preempts the idle/ad timer",async r=>{
    const {p}=r.player();
    r.run(fixtures.youtube);await r.clock.advance(100);
    p.classList.add("ad-showing");
    p.insertAdjacentHTML("beforeend",'<button id="skip-button-new">Skip</button>');
    p.lastElementChild.onclick=()=>{r.calls.clicks++;p.classList.remove("ad-showing");};
    await r.clock.advance(180);
    check(r.calls.clicks===1,"observer did not promptly dismiss");
  });
  await test("DISMISS: wrapper ad state and controls outside the inner player",async r=>{
    const {p}=r.player();
    const wrapper=r.d.createElement("ytd-player");
    p.replaceWith(wrapper);wrapper.appendChild(p);
    r.run(fixtures.youtube);await r.clock.advance(120);
    wrapper.classList.add("ad-showing");
    wrapper.insertAdjacentHTML("beforeend",'<button class="ytp-ad-skip-button-modern">Skip</button>');
    wrapper.lastElementChild.onclick=()=>{r.calls.clicks++;wrapper.classList.remove("ad-showing");};
    await r.clock.advance(180);
    check(r.calls.clicks===1,"wrapper ad state or sibling control ignored");
  });
  await test("END: bounded, spaced ad-clock seeks without rate or mute changes",async r=>{
    const {v}=r.player(true);
    v.playbackRate=1.25;v.muted=true;
    r.run(fixtures.youtube);await r.clock.advance(5000);
    check(r.calls.seeks.length===3,"unexpected seek budget: "+r.calls.seeks.length);
    check(r.calls.seeks.every(x=>x.end===15),"content/guessed clock used");
    for(let i=1;i<r.calls.seeks.length;i++)check(r.calls.seeks[i].at-r.calls.seeks[i-1].at>=1200,"seek flood");
    check(v.playbackRate===1.25&&v.muted===true,"user playback state changed");
  });
  await test("END: unknown/live/content clocks and active seeking never cause blind seeks",async r=>{
    const {p,v}=r.player();
    r.run(fixtures.youtube);await r.clock.advance(100);
    p.classList.add("ad-showing");
    for(const duration of [NaN,Infinity,-1,0,901,900]){
      p.getProgressState=()=>({duration,seekableEnd:20});
      await r.clock.advance(1500);
    }
    p.getProgressState=()=>({duration:15});v.seeking=true;
    await r.clock.advance(1500);
    check(r.calls.seeks.length===0,"unsafe clock or seeking state was ignored");
  });
  await test("RECOVER: only remembered content ID/time, one attempt per video",async r=>{
    const {p,v}=r.player();
    r.run(fixtures.youtube);await r.clock.advance(100);
    p.classList.add("ad-showing");
    p.getVideoData=()=>({video_id:"ADADADADADA"});
    v.currentTime=3;
    await r.clock.advance(80000);
    check(r.calls.reloads.length===1,"recovery loop or missing recovery");
    check(r.calls.reloads[0].id==="AAAAAAAAAAA","recovery loaded an ad ID");
    check(r.calls.reloads[0].start===41.5,"content position overwritten by ad timeline");
  });
  await test("RECOVER: SPA navigation resets position and retains global cooldown",async r=>{
    const {p,v}=r.player();
    r.run(fixtures.youtube);await r.clock.advance(100);
    p.classList.add("ad-showing");await r.clock.advance(6500);
    r.location.href="https://www.youtube.com/watch?v=BBBBBBBBBBB";
    p.getVideoData=()=>({video_id:"ADADADADADA"});v.currentTime=2;
    r.d.dispatchEvent(new r.w.Event("yt-navigate-finish"));
    await r.clock.advance(6000);
    check(r.calls.reloads.length===1,"global cooldown lost on navigation");
    await r.clock.advance(30000);
    check(r.calls.reloads.length===2,"new content did not get its own recovery budget");
    check(r.calls.reloads[1].id==="BBBBBBBBBBB"&&r.calls.reloads[1].start===0,"stale SPA position/ID used");
  });
  await test("RECOVER: unresponsive skip buttons cannot starve later layers",async r=>{
    const {p}=r.player(true);
    for(let i=0;i<12;i++)p.insertAdjacentHTML("beforeend",'<button class="ytp-ad-skip-button-modern">Skip</button>');
    r.run(fixtures.youtube);await r.clock.advance(10000);
    check(r.calls.seeks.length>0,"buttons starved END");
    check(r.calls.reloads.length===1,"buttons starved RECOVER");
  });
  await test("RECOVER: paused playback and live content are left alone",async r=>{
    const {p,v}=r.player(true);
    v.paused=true;
    r.run(fixtures.youtube);await r.clock.advance(8000);
    check(r.calls.reloads.length===0,"paused playback was restarted");
    v.paused=false;p.getVideoData=()=>({video_id:"AAAAAAAAAAA",isLive:true});
    await r.clock.advance(8000);
    check(r.calls.reloads.length===0,"live stream was reloaded");
  });
  await test("runtime: slower idle timer, player-only attributes and guard repair",async r=>{
    const {p}=r.player();
    r.run(fixtures.youtube);await r.clock.advance(120);
    check(r.clock.timers.every(delay=>delay>1000),"idle page still fast-polls");
    check(!r.observations.some(x=>x.document&&x.options.attributes),"document-wide attribute observer");
    check(r.observations.some(x=>x.target===p.id&&x.options.attributeFilter),"player observer not installed");
    const guard=r.d.getElementById("aurora-youtube-player-guard");
    check(guard&&guard.textContent.includes("video"),"player guard missing");
    guard.remove();await r.clock.advance(150);
    check(r.d.getElementById("aurora-youtube-player-guard"),"player guard not restored");
  });
  await test("runtime: replaced player reattaches observer and dismisses",async r=>{
    r.player();r.run(fixtures.youtube);await r.clock.advance(100);
    const {p}=r.player(true);
    p.insertAdjacentHTML("beforeend",'<button class="ytp-ad-skip-button-modern">Skip</button>');
    p.lastElementChild.onclick=()=>{r.calls.clicks++;p.classList.remove("ad-showing");};
    await r.clock.advance(250);
    check(r.calls.clicks===1,"replacement player not observed");
  });
  await test("runtime: hidden idle pages sleep and visibility events preempt the timer",async r=>{
    r.player();r.run(fixtures.youtube);await r.clock.advance(120);
    Object.defineProperty(r.d,"hidden",{configurable:true,value:true});
    r.d.dispatchEvent(new r.w.Event("visibilitychange"));await r.clock.advance(200);
    check(r.clock.timers.every(delay=>delay>4000),"hidden idle page still polls quickly");
    Object.defineProperty(r.d,"hidden",{configurable:true,value:false});
    r.d.dispatchEvent(new r.w.Event("visibilitychange"));await r.clock.advance(200);
    check(r.clock.timers.some(delay=>delay<1500),"visible page did not resume idle checks");
  });
  await test("runtime: load-start installation works before a document root exists",async r=>{
    r.d.documentElement.remove();
    r.run(fixtures.cosmetic);
    r.w.__auroraCosmeticInstalled.update('{"hide_selectors":[".ordinary-ad"]}',[],true);
    r.run(fixtures.youtube);await r.clock.advance(100);
    const root=r.d.createElement("html");
    root.innerHTML='<head></head><body><div id="movie_player" class="html5-video-player ad-showing">'+
      '<video></video><button class="ytp-ad-skip-button">Skip</button></div>'+
      '<div class="ordinary-ad"></div></body>';
    r.d.appendChild(root);
    r.d.querySelector("button").onclick=()=>{r.calls.clicks++;r.d.getElementById("movie_player").classList.remove("ad-showing");};
    await r.clock.advance(250);
    check(r.calls.clicks===1,"early install did not discover the player");
    check(r.d.getElementById("aurora-youtube-player-guard"),"early install lost the player guard");
    check(r.w.getComputedStyle(r.d.querySelector(".ordinary-ad")).display==="none","early cosmetic style missing");
  });
  await test("runtime: explicit SSAP ad signal works without ad-showing class",async r=>{
    const {p}=r.player();
    p.getStatsForNerds=()=>({debug_info:"SSAP, AD, fixture"});
    r.run(fixtures.youtube);await r.clock.advance(3000);
    check(r.calls.seeks.length>0,"explicit SSAP ad signal ignored");
  });
  return {passed:results.filter(x=>x.passed).length,failed:results.filter(x=>!x.passed).length,tests:results};
}
