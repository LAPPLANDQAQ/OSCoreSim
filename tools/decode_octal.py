"""Replace octal escape sequences in C++ source files with UTF-8 Chinese text."""
import re, glob

def decode_octal(match):
    """Decode a sequence of octal bytes to UTF-8 string."""
    octal_bytes = match.group(0)
    # Parse each \ooo into a byte
    bytes_list = []
    i = 0
    while i < len(octal_bytes):
        if octal_bytes[i] == '\\' and i + 3 < len(octal_bytes):
            byte_val = int(octal_bytes[i+1:i+4], 8)
            bytes_list.append(byte_val)
            i += 4
        else:
            bytes_list.append(ord(octal_bytes[i]))
            i += 1
    try:
        return bytes(bytes_list).decode('utf-8')
    except:
        return match.group(0)

def process_file(filepath):
    with open(filepath, 'r', encoding='utf-8') as f:
        content = f.read()

    # Match one or more consecutive \ooo octal escapes
    pattern = re.compile(r'(?:\\[0-7]{3})+')

    new_content = pattern.sub(lambda m: decode_octal(m), content)

    if new_content != content:
        with open(filepath, 'w', encoding='utf-8', newline='') as f:
            f.write(new_content)
        return True
    return False

files = glob.glob('src/**/*.cpp', recursive=True) + glob.glob('src/**/*.h', recursive=True)
updated = 0
for f in files:
    if process_file(f):
        updated += 1
        print(f'UPDATED: {f}')
    else:
        print(f'skip: {f}')
print(f'\nTotal updated: {updated}/{len(files)}')
