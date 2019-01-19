# DirectX11Hook

Template for Visual Studio 2017 that you could use to hook games that use DirectX11. The template also hook the IO as explained in my blog post, required to use properly ImGui. Additional features are going to be added as soon as possible. [@niemand_sec](https://twitter.com/niemand_sec)

![](https://github.com/niemand-sec/DirectX11Hook/raw/master/Vermintide2_ImGUI.gif)

## Features:
- Initial Release
  - DirectX11 Hooking for additional rendering
  - Model Logger
- Second Release (01/19/19)
  - New keys added (DELETE, F10 and F9)
  - Wallhack feature
  - Enable/Disable wallhack option
  - Backup hook for Present method
  - Improvement while hooking Present (no more crashes)
  - Wiping the model list for better usage
  - Compilation/Validation for x86 and x64

## Shortcuts:

- **PGUP**   - Move forward in the model list
- **PGDN**   - Move backward in the model list
- **L**      - Log current model information to console
- **DELETE** - clear model log
- **F10**    - Enable/Disable Wallhack
- **F9**     - Change between Texture and Shader highlighting
- **INSERT** - Display/Hide ImGui Interface

## Requirements:

- VS 2017
- Makes use of detours header and .lib file
- ImGui files (version v1.67)

> Note that the libraries are included inside the project, you should update them when required. Additionally, the offset used to hook the Present method may differ from one game to another, pay attention to that.


## Related Posts:

If you want to customize this template or modify its behavior here you have some blog posts that explain the whole code:

How to Hook DirectX 11 + ImGui (Vermintide 2): 
- https://niemand.com.ar/2019/01/01/how-to-hook-directx-11-imgui/

Fingerprinting Models when hooking DirectX (Vermintide 2):
- https://niemand.com.ar/2019/01/08/fingerprinting-models-when-hooking-directx-vermintide-2/


![](https://github.com/niemand-sec/DirectX11Hook/raw/master/20190113171653_1.jpg)
