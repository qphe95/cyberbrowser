(function() {
  var element = document.querySelector('#player-placeholder');
  if (element && element.remove && typeof element.remove === 'function') {
    element.remove();
  }
})();