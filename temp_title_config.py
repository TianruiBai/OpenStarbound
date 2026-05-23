import json
with open(r'c:\Users\weyst\OpenStarbound\assets\starbound-pack\interface\windowconfig\title.config', 'r') as f:
    data = json.load(f)
print('=== Top-level keys ===')
print(list(data.keys()))
print()
print('=== mainMenuButtons ===')
for btn in data.get('mainMenuButtons', []):
    k = btn.get('key', '?')
    img = btn.get('button', '?')
    print(f'  {k:20s} image={img[:60]}')
print()
print('=== backdropImages count ===')
print(len(data.get('backdropImages', [])))
print()
print('=== scripts ===')
print(data.get('scripts', 'none'))
print()
print('=== music keys ===')
music = data.get('music', {})
if isinstance(music, dict):
    print(list(music.keys())[:5])
