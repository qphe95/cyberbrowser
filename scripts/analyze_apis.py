#!/usr/bin/env python3
"""
Browser API Analysis Tool for YouTube JS Files
Analyzes each JS file and extracts browser APIs that need to be implemented.
"""

import os
import re
import json
from pathlib import Path

# Browser API categories to detect
API_PATTERNS = {
    "Window APIs": {
        "pattern": r'\bwindow\.[a-zA-Z_$][a-zA-Z0-9_$]*(?:\.[a-zA-Z_$][a-zA-Z0-9_$]*)*\b',
        "examples": ["window.location", "window.navigator", "window.fetch"]
    },
    "Document APIs": {
        "pattern": r'\bdocument\.[a-zA-Z_$][a-zA-Z0-9_$]*(?:\.[a-zA-Z_$][a-zA-Z0-9_$]*)*\b',
        "examples": ["document.createElement", "document.querySelector"]
    },
    "Navigator APIs": {
        "pattern": r'\bnavigator\.[a-zA-Z_$][a-zA-Z0-9_$]*(?:\.[a-zA-Z_$][a-zA-Z0-9_$]*)*\b',
        "examples": ["navigator.userAgent", "navigator.mediaCapabilities"]
    },
    "Location APIs": {
        "pattern": r'\blocation\.[a-zA-Z_$][a-zA-Z0-9_$]*\b',
        "examples": ["location.href", "location.hostname"]
    },
    "Console APIs": {
        "pattern": r'\bconsole\.[a-zA-Z_$][a-zA-Z0-9_$]*\b',
        "examples": ["console.log", "console.error"]
    },
    "LocalStorage APIs": {
        "pattern": r'\blocalStorage\.[a-zA-Z_$][a-zA-Z0-9_$]*\b',
        "examples": ["localStorage.getItem", "localStorage.setItem"]
    },
    "SessionStorage APIs": {
        "pattern": r'\bsessionStorage\.[a-zA-Z_$][a-zA-Z0-9_$]*\b',
        "examples": ["sessionStorage.getItem", "sessionStorage.setItem"]
    },
    "History APIs": {
        "pattern": r'\bhistory\.[a-zA-Z_$][a-zA-Z0-9_$]*\b',
        "examples": ["history.pushState", "history.replaceState"]
    },
    "Screen APIs": {
        "pattern": r'\bscreen\.[a-zA-Z_$][a-zA-Z0-9_$]*\b',
        "examples": ["screen.width", "screen.height"]
    },
    "Performance APIs": {
        "pattern": r'\bperformance\.[a-zA-Z_$][a-zA-Z0-9_$]*(?:\.[a-zA-Z_$][a-zA-Z0-9_$]*)*\b',
        "examples": ["performance.now", "performance.memory"]
    },
    "Fetch API": {
        "pattern": r'\bfetch\s*\(',
        "examples": ["fetch()"]
    },
    "XMLHttpRequest": {
        "pattern": r'\bXMLHttpRequest\b',
        "examples": ["new XMLHttpRequest()"]
    },
    "WebSocket": {
        "pattern": r'\bWebSocket\b',
        "examples": ["new WebSocket()"]
    },
    "Worker APIs": {
        "pattern": r'\bWorker\b|\bSharedWorker\b',
        "examples": ["new Worker()", "new SharedWorker()"]
    },
    "Observer APIs": {
        "pattern": r'\bMutationObserver\b|\bIntersectionObserver\b|\bResizeObserver\b',
        "examples": ["new MutationObserver()", "new IntersectionObserver()"]
    },
    "EventTarget APIs": {
        "pattern": r'\baddEventListener\s*\(|\bremoveEventListener\s*\(|\bdispatchEvent\s*\(',
        "examples": [".addEventListener()", ".removeEventListener()"]
    },
    "Timeout/Interval": {
        "pattern": r'\bsetTimeout\s*\(|\bsetInterval\s*\(|\bclearTimeout\s*\(|\bclearInterval\s*\(',
        "examples": ["setTimeout()", "setInterval()"]
    },
    "RequestAnimationFrame": {
        "pattern": r'\brequestAnimationFrame\s*\(|\bcancelAnimationFrame\s*\(',
        "examples": ["requestAnimationFrame()"]
    },
    "MediaSource APIs": {
        "pattern": r'\bMediaSource\b|\bManagedMediaSource\b|\bWebKitMediaSource\b',
        "examples": ["new MediaSource()", "new ManagedMediaSource()"]
    },
    "URL APIs": {
        "pattern": r'\bURL\.[a-zA-Z_$][a-zA-Z0-9_$]*\b',
        "examples": ["URL.createObjectURL()", "URL.revokeObjectURL()"]
    },
    "Custom Elements": {
        "pattern": r'\bcustomElements\.[a-zA-Z_$][a-zA-Z0-9_$]*\b',
        "examples": ["customElements.define()", "customElements.get()"]
    },
    "Promise APIs": {
        "pattern": r'\bPromise\.[a-zA-Z_$][a-zA-Z0-9_$]*\b',
        "examples": ["Promise.resolve()", "Promise.all()"]
    },
    "JSON APIs": {
        "pattern": r'\bJSON\.[a-zA-Z_$][a-zA-Z0-9_$]*\b',
        "examples": ["JSON.parse()", "JSON.stringify()"]
    },
    "Object APIs": {
        "pattern": r'\bObject\.[a-zA-Z_$][a-zA-Z0-9_$]*\b',
        "examples": ["Object.defineProperty()", "Object.create()"]
    },
    "Array Method Calls": {
        "pattern": r'\b(?:forEach|map|filter|reduce|find|some|every|includes|indexOf|push|pop|shift|unshift|splice|slice|concat|join|sort|reverse)\s*\(',
        "examples": [".forEach()", ".map()", ".filter()"]
    },
    "Crypto APIs": {
        "pattern": r'\bcrypto\.[a-zA-Z_$][a-zA-Z0-9_$]*\b|\bCrypto\b',
        "examples": ["crypto.getRandomValues()", "window.crypto"]
    },
    "AbortController": {
        "pattern": r'\bAbortController\b|\bAbortSignal\b',
        "examples": ["new AbortController()"]
    },
    "HTML Element Constructors": {
        "pattern": r'\bHTML[a-zA-Z]*Element\b',
        "examples": ["HTMLVideoElement", "HTMLMediaElement"]
    },
    "Event Constructors": {
        "pattern": r'\bnew\s+(?:Event|CustomEvent|MouseEvent|KeyboardEvent|TouchEvent|MessageEvent)\b',
        "examples": ["new Event()", "new CustomEvent()"]
    },
    "FormData": {
        "pattern": r'\bFormData\b',
        "examples": ["new FormData()"]
    },
    "Blob/File APIs": {
        "pattern": r'\bBlob\b|\bFile\b|\bFileReader\b',
        "examples": ["new Blob()", "new FileReader()"]
    },
    "Image APIs": {
        "pattern": r'\bnew\s+Image\s*\(|\bImage\s*\(',
        "examples": ["new Image()"]
    },
    "DOMParser": {
        "pattern": r'\bDOMParser\b',
        "examples": ["new DOMParser()"]
    },
    "XMLSerializer": {
        "pattern": r'\bXMLSerializer\b',
        "examples": ["new XMLSerializer()"]
    },
    "matchMedia": {
        "pattern": r'\bmatchMedia\s*\(',
        "examples": ["window.matchMedia()"]
    },
    "btoa/atob": {
        "pattern": r'\bbtoa\s*\(|\batob\s*\(',
        "examples": ["btoa()", "atob()"]
    },
    "encodeURIComponent": {
        "pattern": r'\bencodeURIComponent\s*\(|\bdecodeURIComponent\s*\(',
        "examples": ["encodeURIComponent()", "decodeURIComponent()"]
    },
    "escape/unescape": {
        "pattern": r'\bescape\s*\(|\bunescape\s*\(',
        "examples": ["escape()", "unescape()"]
    }
}

def analyze_file(filepath):
    """Analyze a single JS file for browser APIs."""
    try:
        with open(filepath, 'r', encoding='utf-8', errors='ignore') as f:
            content = f.read()
    except Exception as e:
        return {"error": str(e)}
    
    results = {
        "filename": os.path.basename(filepath),
        "file_size": len(content),
        "apis_found": {},
        "summary": {
            "total_unique_apis": 0,
            "categories_with_apis": []
        }
    }
    
    all_apis = set()
    
    for category, config in API_PATTERNS.items():
        pattern = config["pattern"]
        matches = re.findall(pattern, content)
        
        # Clean and deduplicate matches
        unique_matches = sorted(set(matches))
        
        if unique_matches:
            results["apis_found"][category] = {
                "count": len(unique_matches),
                "apis": unique_matches[:50]  # Limit to first 50 to avoid huge files
            }
            results["summary"]["categories_with_apis"].append(category)
            all_apis.update(unique_matches)
    
    results["summary"]["total_unique_apis"] = len(all_apis)
    
    return results

def generate_analysis_text(results):
    """Generate human-readable analysis text."""
    lines = []
    lines.append("=" * 80)
    lines.append(f"BROWSER API ANALYSIS: {results['filename']}")
    lines.append("=" * 80)
    lines.append("")
    lines.append(f"File Size: {results['file_size']:,} bytes")
    lines.append(f"Total Unique APIs Found: {results['summary']['total_unique_apis']}")
    lines.append(f"Categories with APIs: {len(results['summary']['categories_with_apis'])}")
    lines.append("")
    
    if "error" in results:
        lines.append(f"ERROR: {results['error']}")
        return "\n".join(lines)
    
    # Priority APIs first
    priority_categories = [
        "Window APIs", "Document APIs", "Navigator APIs", "Fetch API",
        "XMLHttpRequest", "MediaSource APIs", "EventTarget APIs",
        "Timeout/Interval", "Performance APIs", "Observer APIs"
    ]
    
    # Sort categories: priority first, then alphabetically
    sorted_categories = []
    for cat in priority_categories:
        if cat in results["apis_found"]:
            sorted_categories.append(cat)
    for cat in sorted(results["apis_found"].keys()):
        if cat not in sorted_categories:
            sorted_categories.append(cat)
    
    for category in sorted_categories:
        info = results["apis_found"][category]
        lines.append("-" * 80)
        lines.append(f"{category} ({info['count']} unique)")
        lines.append("-" * 80)
        
        for api in info["apis"]:
            lines.append(f"  - {api}")
        lines.append("")
    
    lines.append("=" * 80)
    lines.append("END OF ANALYSIS")
    lines.append("=" * 80)
    
    return "\n".join(lines)

def main():
    script_dir = Path(__file__).parent.resolve()
    data_dir = script_dir / "youtube_data"
    analysis_dir = script_dir / "youtube_data_analysis"
    
    # Get all JS files
    js_files = sorted(data_dir.glob("youtube_script_*.js"))
    
    print(f"Analyzing {len(js_files)} JavaScript files...")
    print()
    
    all_apis_summary = {}
    
    for i, js_file in enumerate(js_files, 1):
        print(f"  [{i}/{len(js_files)}] Analyzing {js_file.name}...")
        
        # Analyze the file
        results = analyze_file(js_file)
        
        # Generate analysis text
        analysis_text = generate_analysis_text(results)
        
        # Save analysis
        analysis_filename = js_file.stem + "_analysis.txt"
        analysis_path = analysis_dir / analysis_filename
        
        with open(analysis_path, 'w', encoding='utf-8') as f:
            f.write(analysis_text)
        
        # Track for summary
        all_apis_summary[js_file.name] = {
            "apis_count": results["summary"]["total_unique_apis"],
            "categories": results["summary"]["categories_with_apis"]
        }
    
    # Generate overall summary
    print()
    print("Generating summary report...")
    
    summary_lines = []
    summary_lines.append("=" * 80)
    summary_lines.append("OVERALL BROWSER API ANALYSIS SUMMARY")
    summary_lines.append("=" * 80)
    summary_lines.append("")
    summary_lines.append(f"Total Files Analyzed: {len(js_files)}")
    summary_lines.append("")
    
    # Count all unique APIs across all files
    all_unique_apis = set()
    api_usage_count = {}
    
    for js_file in js_files:
        results = analyze_file(js_file)
        if "apis_found" in results:
            for category, info in results["apis_found"].items():
                for api in info["apis"]:
                    all_unique_apis.add(api)
                    api_usage_count[api] = api_usage_count.get(api, 0) + 1
    
    summary_lines.append(f"Total Unique APIs Across All Files: {len(all_unique_apis)}")
    summary_lines.append("")
    
    # Top 50 most used APIs
    summary_lines.append("-" * 80)
    summary_lines.append("TOP 50 MOST FREQUENTLY USED APIs")
    summary_lines.append("-" * 80)
    summary_lines.append("")
    
    sorted_apis = sorted(api_usage_count.items(), key=lambda x: x[1], reverse=True)
    for i, (api, count) in enumerate(sorted_apis[:50], 1):
        summary_lines.append(f"  {i:2}. {api:<50} (used in {count} files)")
    
    summary_lines.append("")
    summary_lines.append("-" * 80)
    summary_lines.append("FILES SUMMARY")
    summary_lines.append("-" * 80)
    summary_lines.append("")
    
    for filename, info in sorted(all_apis_summary.items()):
        summary_lines.append(f"{filename}")
        summary_lines.append(f"  APIs: {info['apis_count']}, Categories: {len(info['categories'])}")
        if info['categories']:
            summary_lines.append(f"  Main categories: {', '.join(info['categories'][:5])}")
        summary_lines.append("")
    
    summary_lines.append("=" * 80)
    summary_lines.append("END OF SUMMARY")
    summary_lines.append("=" * 80)
    
    summary_text = "\n".join(summary_lines)
    
    # Save summary
    summary_path = analysis_dir / "00_OVERALL_SUMMARY.txt"
    with open(summary_path, 'w', encoding='utf-8') as f:
        f.write(summary_text)
    
    # Also save as JSON for programmatic access
    json_path = analysis_dir / "00_api_summary.json"
    with open(json_path, 'w', encoding='utf-8') as f:
        json.dump({
            "total_files": len(js_files),
            "total_unique_apis": len(all_unique_apis),
            "api_usage_frequency": sorted_apis,
            "file_summaries": all_apis_summary
        }, f, indent=2)
    
    print()
    print("Analysis complete!")
    print(f"  Analysis files saved to: {analysis_dir}")
    print(f"  Overall summary: {summary_path}")
    print(f"  JSON data: {json_path}")
    print()
    print(f"Total unique APIs found: {len(all_unique_apis)}")

if __name__ == "__main__":
    main()
