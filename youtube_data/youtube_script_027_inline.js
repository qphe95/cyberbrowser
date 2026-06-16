(function() {
  var element = document.querySelector('#player-api');
  if (yt && yt.flexy && yt.flexy.setPlayerlikeElementSize && typeof yt.flexy.setPlayerlikeElementSize === 'function') {
    yt.flexy.setPlayerlikeElementSize(element);
  }
})();