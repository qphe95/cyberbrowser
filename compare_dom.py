#!/usr/bin/env python3
import sys
from html.parser import HTMLParser
from collections import Counter

class TagCounter(HTMLParser):
    def __init__(self):
        super().__init__()
        self.tags = Counter()
        self.ytd_tags = Counter()
        self.imgs = 0
        self.imgs_src = []
        self.ids = Counter()
        self.custom_elements = 0
        self.total = 0
    def handle_starttag(self, tag, attrs):
        self.total += 1
        self.tags[tag] += 1
        if tag.startswith('ytd-') or tag.startswith('yt-'):
            self.ytd_tags[tag] += 1
            self.custom_elements += 1
        if tag == 'img':
            self.imgs += 1
            src = next((v for k, v in attrs if k == 'src'), '')
            if src:
                self.imgs_src.append(src)
        idv = next((v for k, v in attrs if k == 'id'), '')
        if idv:
            self.ids[idv] += 1
    def handle_startendtag(self, tag, attrs):
        self.handle_starttag(tag, attrs)

def analyze(path):
    with open(path, 'r', encoding='utf-8', errors='ignore') as f:
        data = f.read()
    p = TagCounter()
    p.feed(data)
    return p

if __name__ == '__main__':
    cyber = analyze(sys.argv[1])
    chrome = analyze(sys.argv[2])
    print('=== totals ===')
    print(f'cyber total: {cyber.total}, img: {cyber.imgs}, custom: {cyber.custom_elements}')
    print(f'chrome total: {chrome.total}, img: {chrome.imgs}, custom: {chrome.custom_elements}')
    print('\n=== top tags cyber ===')
    for t, c in cyber.tags.most_common(20):
        cc = chrome.tags.get(t, 0)
        print(f'{t}: {c} (chrome: {cc})')
    print('\n=== top tags chrome ===')
    for t, c in chrome.tags.most_common(20):
        cc = cyber.tags.get(t, 0)
        if c != cc:
            print(f'{t}: {c} (cyber: {cc})')
    print('\n=== ytd tags cyber ===')
    for t, c in sorted(cyber.ytd_tags.items(), key=lambda x: -x[1]):
        cc = chrome.ytd_tags.get(t, 0)
        print(f'{t}: {c} (chrome: {cc})')
    print('\n=== ytd tags chrome (missing in cyber) ===')
    for t, c in sorted(chrome.ytd_tags.items(), key=lambda x: -x[1]):
        cc = cyber.ytd_tags.get(t, 0)
        if cc == 0:
            print(f'{t}: {c}')
    print('\n=== ids ===')
    for i, c in sorted(cyber.ids.items(), key=lambda x: -x[1]):
        cc = chrome.ids.get(i, 0)
        if c != cc:
            print(f'#{i}: {c} (chrome: {cc})')
    print('\n=== chrome ids not in cyber ===')
    for i, c in sorted(chrome.ids.items(), key=lambda x: -x[1]):
        cc = cyber.ids.get(i, 0)
        if cc == 0:
            print(f'#{i}: {c}')
