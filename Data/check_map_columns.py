with open('10_18_run5_001.csv', 'r', encoding='latin-1') as f:
    lines = f.readlines()

print("First 20 lines of file:")
print("="*80)
for i, line in enumerate(lines[:20]):
    print(f"Line {i}: {line[:150]}")

print("\n\nLooking for header with MAP:")
print("="*80)
for i, line in enumerate(lines):
    if 'MAP' in line:
        print(f"Found MAP at line {i}:")
        print(line)
        break
