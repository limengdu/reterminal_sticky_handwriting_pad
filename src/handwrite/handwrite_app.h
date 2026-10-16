#pragma once

class Canvas;

// Runs the handwriting notebook forever: finger strokes draw ink, the side
// buttons turn pages, a short AI-button click clears the page, and a long
// press runs a full refresh to remove e-paper ghosting.
// 运行手写笔记本主循环：手指划过即书写，侧键翻页，AI键短按清空当前页，
// 长按执行一次全刷以消除电子纸残影。该函数不会返回。
[[noreturn]] void handwrite_app_run(Canvas &canvas);
