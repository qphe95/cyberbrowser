var t0 = Date.now();
requestIdleCallback(function(deadline){
  console.error("IDLE FIRED after " + (Date.now() - t0) + "ms didTimeout=" + deadline.didTimeout + " timeRemaining=" + deadline.timeRemaining());
}, {timeout: 300});
requestIdleCallback(function(){
  console.error("IDLE2 fired after " + (Date.now() - t0) + "ms");
});
setTimeout(function(){ console.error("TIMEOUT 500ms fired after " + (Date.now() - t0)); }, 500);
