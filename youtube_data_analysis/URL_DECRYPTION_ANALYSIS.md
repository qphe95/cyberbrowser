# YouTube URL Signature Decryption Analysis

## Overview

YouTube encrypts video stream URLs using a `signatureCipher` that must be decrypted before the video can be played. This analysis documents how the decryption works based on the downloaded scripts.

---

## Signature Cipher Structure

### Raw signatureCipher Format

```
s=u%3DQYwPBX1vv95916s7qJn9vQQAxGWFyZbhcxKmZtiNQxTAiAYX3r4Dw%3D1q7S55H-t2Qx5hARADEOOuMPWzxO_3sxCrJAI-QRw4MNqEHIEHh&sp=sig&url=https://rr4---sn-a5meknzr.googlevideo.com/videoplayback%3Fexpire%3D...
```

### URL-Decoded Components

| Parameter | Value | Description |
|-----------|-------|-------------|
| `s` | `u=QYwPBX1vv95916s7qJn9vQQAxGWFyZbhcxKmZtiNQxTAiAYX3r4Dw=1q7S55H-t2Qx5hARADEOOuMPWzxO_3sxCrJAI-QRw4MNqEHIEHh` | **Encrypted signature** |
| `sp` | `sig` | **Signature parameter name** - the decrypted sig will be added as `&sig=...` |
| `url` | `https://rr4---sn-a5meknzr.googlevideo.com/videoplayback?expire=...` | **Base URL** without signature |

### Final Decrypted URL Format

```
https://rr4---sn-a5meknzr.googlevideo.com/videoplayback?expire=1773910313&ei=...&ip=...&id=...&sig=<DECRYPTED_SIGNATURE>
```

---

## How Signature Decryption Works

### 1. The Decryption Algorithm

The signature decryption involves a **sequence of string operations**:

```javascript
// Pseudocode of typical YouTube sig decipher
function decipherSignature(encryptedSig) {
    // Step 1: Convert to array
    let a = encryptedSig.split("");
    
    // Step 2: Apply transformation operations
    a = reverseArray(a);           // Reverse
    a = sliceArray(a, 3);          // Slice from index 3
    a = spliceArray(a, 1, 2);      // Remove 2 elements at index 1
    a = swapElements(a, 0, 5);     // Swap elements at indices
    // ... more operations
    
    // Step 3: Join back to string
    return a.join("");
}
```

### 2. Operation Types

Based on the player code analysis, the operations are:

| Operation | JavaScript | Effect |
|-----------|------------|--------|
| Reverse | `a.reverse()` | Reverses the entire array |
| Slice | `a.slice(N)` | Keeps elements from index N onwards |
| Splice | `a.splice(N, M)` | Removes M elements at index N |
| Swap | `a[0]=a[N]` | Swaps first element with element at N |

### 3. The Challenge: Dynamic Algorithm

**Critical**: The sequence of operations **changes regularly** (weekly/daily). YouTube updates the player code with new transformation sequences.

---

## Where the Decryption Code Lives

### In the Player (base.js)

File: `youtube_script_037_external.js` (9.5MB - main player)

The decryption functions are:
1. **Heavily obfuscated** - function names are randomized (e.g., `a`, `b`, `G`, `r`)
2. **Dynamically generated** - varies with each player update
3. **Split across multiple functions** - operations are distributed

### Finding the Decryption Function

In the player code, look for:

```javascript
// Pattern 1: Function taking single string parameter
function(a) { a = a.split(""); ...; return a.join(""); }

// Pattern 2: Object with multiple transform functions
var transforms = {
    "a": function(a) { a.reverse() },
    "b": function(a, b) { a.splice(0, b) },
    // ...
};
```

### The "STS" (Signature Timestamp)

```javascript
// In player code - signature timestamp
f.signatureTimestamp = _.nL("STS");  // Line 37976 in base.js
```

The STS is used to:
- Identify the player version
- Validate that the client has a compatible player
- Potentially select the correct decryption algorithm

---

## Decryption Process Steps

### Step 1: Extract signatureCipher from ytInitialPlayerResponse

```javascript
// From ytInitialPlayerResponse.streamingData.formats
{
    "itag": 18,
    "mimeType": "video/mp4; codecs=\"avc1.42001E, mp4a.40.2\"",
    "signatureCipher": "s=...&sp=sig&url=..."
}
```

### Step 2: Parse the cipher

```javascript
const cipher = format.signatureCipher;
const params = new URLSearchParams(cipher);

const encryptedSig = params.get('s');        // Encrypted signature
const sigParam = params.get('sp') || 'sig';  // Usually 'sig'
const baseUrl = params.get('url');           // Base URL
```

### Step 3: Execute the decipher function

The decipher function must be:
1. Extracted from the player JavaScript
2. Parsed to understand the operation sequence
3. Executed on the encrypted signature

### Step 4: Construct the final URL

```javascript
const finalUrl = `${baseUrl}&${sigParam}=${decryptedSig}`;
```

---

## Implementation Requirements

### What You Need to Implement

1. **JavaScript Parser/Executor**
   - Parse the obfuscated player code
   - Extract the decipher function
   - Execute it in a sandboxed JavaScript environment

2. **Pattern Recognition**
   - Identify the transformation sequence
   - Handle dynamic function name changes

3. **Caching**
   - Cache the decipher function for the STS lifetime
   - Avoid re-parsing the player for every URL

### QuickJS Implementation Approach

```c
// 1. Load the player JS into QuickJS
// 2. Extract the decipher function by pattern matching
// 3. Call the function with the encrypted signature
// 4. Get the decrypted result

// Example C code:
JSValue decipher_func = JS_GetPropertyStr(ctx, global_obj, "decipher");
JSValue encrypted_sig = JS_NewString(ctx, encrypted);
JSValue result = JS_Call(ctx, decipher_func, JS_UNDEFINED, 1, &encrypted_sig);
const char *decrypted = JS_ToCString(ctx, result);
```

---

## File References

### Key Files for Decryption Analysis

| File | Lines | Contains |
|------|-------|----------|
| `youtube_script_037_external.js` | ~650K lines | Main player with decipher functions |
| `youtube_script_024_external.js` | ~95K lines | Player base code |
| `youtube_page.html` | - | ytInitialPlayerResponse with ciphers |

### Relevant Code Patterns

In `youtube_script_037_external.js`:
```javascript
// Line ~37976: STS handling
isNaN(_.nL("STS")) || (f.signatureTimestamp = _.nL("STS"));

// Line ~190083: STS in headers
"X-YouTube-STS": _.nL("STS").toString()
```

In `youtube_script_024_external.js`:
```javascript
// Obfuscated string array containing cipher-related strings
var n = 'split;;scheme;L;call;/videoplayback;X;reverse;(}\";path;fromCharCode;slice;...signatureCipher...'.split(";");
```

---

## Challenges & Considerations

### 1. Obfuscation

YouTube uses multiple obfuscation techniques:
- **String array encoding**: Strings stored in arrays, accessed by index
- **Control flow flattening**: Switch statements for control flow
- **Dead code insertion**: Non-functional code to confuse analysis
- **Randomized naming**: Variable/function names change each build

### 2. Dynamic Updates

- Player JS updates frequently (daily/weekly)
- Decryption algorithm changes with each update
- STS timestamp identifies player version

### 3. Security Measures

- **BotGuard**: Additional JavaScript challenge-response
- **IP binding**: Signatures may be bound to requesting IP
- **Time limits**: URLs expire after a short time

---

## Testing Decryption

### Sample Cipher from Downloaded Data

```
s=u=QYwPBX1vv95916s7qJn9vQQAxGWFyZbhcxKmZtiNQxTAiAYX3r4Dw=1q7S55H-t2Qx5hARADEOOuMPWzxO_3sxCrJAI-QRw4MNqEHIEHh
sp=sig
url=https://rr4---sn-a5meknzr.googlevideo.com/videoplayback?expire=1773910313...
```

### Verification

1. Decrypt the signature using the player code
2. Append `&sig=<decrypted>` to the base URL
3. Test the URL with HTTP HEAD request
4. Should return HTTP 200 (not 403)

---

## Summary

To implement URL decryption in bgmdwnldr:

1. **Load player JS** (base.js) into QuickJS runtime
2. **Extract decipher function** using pattern matching or AST analysis
3. **Cache the function** keyed by STS timestamp
4. **For each encrypted URL**:
   - Parse signatureCipher
   - Execute decipher function on the 's' parameter
   - Construct final URL with decrypted signature
   - Use the URL for downloading

**Note**: This is a simplified overview. The actual implementation requires careful handling of the obfuscated JavaScript and regular updates when YouTube changes the algorithm.
