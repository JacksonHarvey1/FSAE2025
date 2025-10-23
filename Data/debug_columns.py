with open('10_18_run1_001.csv', 'r', encoding='latin-1') as f:
    lines = f.readlines()

header_line = next(i for i, line in enumerate(lines) if line.startswith('Time'))
header = lines[header_line]

print("Full header line:")
print(repr(header))
print("\nSplit by comma:")
cols = header.split(',')
for i, col in enumerate(cols):
    print(f"{i}: '{col.strip()}'")
