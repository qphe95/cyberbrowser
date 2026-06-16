#!/usr/bin/env python3
"""
YouTube Signature Decipher Extractor

This script attempts to extract and analyze the signature decipher function
from YouTube's player JavaScript files.
"""

import re
import json
from pathlib import Path

class DecipherExtractor:
    def __init__(self, player_js_path):
        self.player_js = Path(player_js_path).read_text(encoding='utf-8', errors='ignore')
        
    def find_string_table(self):
        """Find the string table used for obfuscation."""
        # Look for string array patterns
        pattern = r'var\s+\w+\s*=\s*[\'"]([^\'"]+)[\'"]\.split\([\'"]([^\'"]+)[\'"]\)'
        matches = re.findall(pattern, self.player_js[:100000])
        
        tables = []
        for strings, delimiter in matches:
            if len(strings) > 100 and delimiter in [';', ',', '|']:
                tables.append({
                    'delimiter': delimiter,
                    'strings': strings.split(delimiter)[:50]  # First 50 entries
                })
        return tables
    
    def find_signature_functions(self):
        """Find functions that likely handle signature decryption."""
        candidates = []
        
        # Pattern 1: Functions that manipulate arrays with common sig operations
        pattern1 = r'function\s+(\w+)\s*\(\s*([a-zA-Z_$])\s*\)\s*\{\s*\2\s*=\s*\2\.split\s*\(\s*["\']\s*["\']\s*\)'
        matches1 = re.finditer(pattern1, self.player_js)
        
        for match in matches1:
            func_name = match.group(1)
            param_name = match.group(2)
            start_pos = match.start()
            
            # Extract function body (simplified - find matching braces)
            brace_count = 1
            pos = match.end()
            while brace_count > 0 and pos < len(self.player_js):
                if self.player_js[pos] == '{':
                    brace_count += 1
                elif self.player_js[pos] == '}':
                    brace_count -= 1
                pos += 1
            
            body = self.player_js[match.start():pos]
            
            # Check if body contains signature operations
            operations = []
            if '.reverse()' in body:
                operations.append('reverse')
            if '.slice(' in body:
                operations.append('slice')
            if '.splice(' in body:
                operations.append('splice')
            if '.join(' in body:
                operations.append('join')
                
            if len(operations) >= 3:  # Likely a sig function
                candidates.append({
                    'name': func_name,
                    'param': param_name,
                    'operations': operations,
                    'body_length': len(body),
                    'snippet': body[:500]
                })
        
        return candidates
    
    def find_transform_object(self):
        """Find the object containing transformation functions."""
        # Look for objects with common transform operation names
        pattern = r'(var\s+)?([a-zA-Z_$][a-zA-Z0-9_$]*)\s*=\s*\{[^}]*\b[a-zA-Z]\s*:\s*function\s*\([^)]*\)\s*\{[^}]*\}'
        matches = re.finditer(pattern, self.player_js[:500000])
        
        objects = []
        for match in matches:
            obj_name = match.group(2)
            obj_content = match.group(0)
            
            # Check if it looks like a transform object
            if len(obj_content) > 200 and len(obj_content) < 5000:
                # Count function definitions
                func_count = obj_content.count('function')
                if func_count >= 3:
                    objects.append({
                        'name': obj_name,
                        'functions': func_count,
                        'snippet': obj_content[:800]
                    })
        
        return objects
    
    def find_sts(self):
        """Find the Signature Timestamp (STS) value."""
        # Look for STS definition
        pattern = r'STS["\']?\s*:\s*(\d+)'
        match = re.search(pattern, self.player_js)
        if match:
            return int(match.group(1))
        
        # Alternative pattern
        pattern2 = r'\.STS\s*=\s*(\d+)'
        match2 = re.search(pattern2, self.player_js)
        if match2:
            return int(match2.group(1))
            
        return None
    
    def analyze(self):
        """Run full analysis."""
        print("=" * 80)
        print("YouTube Signature Decipher Analysis")
        print("=" * 80)
        print()
        
        # Find STS
        sts = self.find_sts()
        print(f"Signature Timestamp (STS): {sts}")
        print()
        
        # Find string tables
        print("-" * 80)
        print("String Tables Found:")
        print("-" * 80)
        tables = self.find_string_table()
        for i, table in enumerate(tables[:3], 1):
            print(f"\nTable {i} (delimiter: '{table['delimiter']}'):")
            print(f"  First 10 entries: {table['strings'][:10]}")
        
        # Find signature functions
        print()
        print("-" * 80)
        print("Potential Signature Functions:")
        print("-" * 80)
        funcs = self.find_signature_functions()
        for i, func in enumerate(funcs[:5], 1):
            print(f"\nCandidate {i}: {func['name']}")
            print(f"  Parameter: {func['param']}")
            print(f"  Operations: {', '.join(func['operations'])}")
            print(f"  Body length: {func['body_length']} chars")
            print(f"  Snippet:")
            print("  " + "\n  ".join(func['snippet'].split('\n')[:10]))
        
        # Find transform objects
        print()
        print("-" * 80)
        print("Potential Transform Objects:")
        print("-" * 80)
        objects = self.find_transform_object()
        for i, obj in enumerate(objects[:3], 1):
            print(f"\nObject {i}: {obj['name']}")
            print(f"  Functions: {obj['functions']}")
            print(f"  Snippet:")
            print("  " + "\n  ".join(obj['snippet'].split('\n')[:15]))
        
        # Save results
        results = {
            'sts': sts,
            'string_tables': len(tables),
            'sig_functions': [
                {
                    'name': f['name'],
                    'operations': f['operations'],
                    'body': f['snippet'][:1000]
                }
                for f in funcs[:3]
            ],
            'transform_objects': [
                {
                    'name': o['name'],
                    'functions': o['functions'],
                    'snippet': o['snippet'][:1000]
                }
                for o in objects[:3]
            ]
        }
        
        output_path = Path(__file__).parent / 'decipher_analysis.json'
        with open(output_path, 'w') as f:
            json.dump(results, f, indent=2)
        
        print()
        print("=" * 80)
        print(f"Results saved to: {output_path}")
        print("=" * 80)

def test_cipher_parsing():
    """Test parsing of signatureCipher from the downloaded data."""
    html_path = Path(__file__).parent.parent / 'youtube_data' / 'youtube_page.html'
    
    if not html_path.exists():
        print(f"HTML file not found: {html_path}")
        return
    
    html = html_path.read_text(encoding='utf-8', errors='ignore')
    
    # Find ytInitialPlayerResponse
    match = re.search(r'ytInitialPlayerResponse\s*=\s*({.+?});', html, re.DOTALL)
    if not match:
        print("ytInitialPlayerResponse not found")
        return
    
    try:
        data = json.loads(match.group(1))
    except json.JSONDecodeError as e:
        print(f"JSON parse error: {e}")
        return
    
    print("=" * 80)
    print("Sample Signature Cipher Analysis")
    print("=" * 80)
    print()
    
    if 'streamingData' in data and 'formats' in data['streamingData']:
        for fmt in data['streamingData']['formats']:
            if 'signatureCipher' in fmt:
                from urllib.parse import parse_qs, unquote
                
                cipher = fmt['signatureCipher']
                parsed = parse_qs(cipher)
                
                print(f"Format itag: {fmt.get('itag')}")
                print(f"MIME type: {fmt.get('mimeType')}")
                print()
                print("Encrypted Signature (s):")
                sig = unquote(parsed.get('s', [''])[0])
                print(f"  Length: {len(sig)}")
                print(f"  Value: {sig}")
                print()
                print(f"Signature Parameter (sp): {unquote(parsed.get('sp', ['sig'])[0])}")
                print()
                print("Base URL:")
                url = unquote(parsed.get('url', [''])[0])
                print(f"  {url[:100]}...")
                print()
                print("Required Operations:")
                print("  1. Decrypt the 's' parameter using player's decipher function")
                print("  2. Append '&sig=<decrypted>' to the base URL")
                print()
                break

if __name__ == "__main__":
    import sys
    
    # Test cipher parsing
    test_cipher_parsing()
    print()
    
    # Analyze player
    player_path = Path(__file__).parent.parent / 'youtube_data' / 'youtube_script_037_external.js'
    
    if player_path.exists():
        extractor = DecipherExtractor(player_path)
        extractor.analyze()
    else:
        print(f"Player file not found: {player_path}")
        print("Make sure you've downloaded the YouTube data first.")
