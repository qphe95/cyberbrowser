if (localStorage.length !== 0) throw new Error('CLEAR_NOT_PERSISTED ' + localStorage.length);
localStorage.setItem('a', '1'); localStorage.setItem('b', '2'); localStorage.setItem('c', '3');
