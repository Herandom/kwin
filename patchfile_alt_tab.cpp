// Here is the needed patch to Kwin to achieve this, build your version then use /usr/local/bin/kwin_x11 --replace to test it.
void FocusChain::makeFirstInChain(AbstractClient *client, Chain &chain)
{
    chain.removeAll(client);
//    if (client->isMinimized()) { // add it before the first minimized ...
//        for (int i = chain.count()-1; i >= 0; --i) {
//            if (chain.at(i)->isMinimized()) {
//                chain.insert(i+1, client);
//                return;
//            }
//        }
//        chain.prepend(client); // ... or at end of chain
//    } else {
        chain.append(client);
//    }
}
