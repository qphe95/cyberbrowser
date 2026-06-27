import re
s = open('build-mingw/youtube_loaded.html', encoding='utf-8').read()
# find index of getInitialData definition
idx = s.find('d.getInitialData=function(){')
print('idx', idx)
# extract from there until the next matching } after the function body (balance braces)
start = idx + len('d.getInitialData=function(){')
depth = 1
i = start
while i < len(s) and depth > 0:
    if s[i] == '{': depth += 1
    elif s[i] == '}': depth -= 1
    i += 1
print(s[idx:i])
