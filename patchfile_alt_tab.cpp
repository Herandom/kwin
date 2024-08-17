// Here is the needed patch to Kwin to achieve this, build your version then use /usr/local/bin/kwin_x11 --replace to test it.
// https://github.com/Intika-Linux-KDE/kwin-classic-ctrl-tab-switcher/commit/47d6ab59eb8914ddb0857382f2d42dff8bb11402
void FocusChain::makeFirstInChain(Window *window, Chain &chain)
{
    chain.removeAll(window);
//    if (window->isMinimized()) { // add it before the first minimized ...
//        for (int i = chain.count()-1; i >= 0; --i) {
//            if (chain.at(i)->isMinimized()) {
//                chain.insert(i+1, window);
//                return;
//            }
//        }
//        chain.prepend(window); // ... or at end of chain
//    } else {
        chain.append(window);
//    }
}
