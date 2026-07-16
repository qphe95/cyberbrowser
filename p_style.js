var d = document.createElement('div');
if (typeof d.style === 'undefined') throw new Error('createElement div: style undefined');
d.style.width = '10px';
var b = document.getElementById('player-api');
if (b && typeof b.style === 'undefined') throw new Error('player-api: style undefined');
var v = document.createElement('video');
if (typeof v.style === 'undefined') throw new Error('video: style undefined');
