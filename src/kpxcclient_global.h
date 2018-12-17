#ifndef KEEPASSXCCLIENTLIBRARY_GLOBAL_H
#define KEEPASSXCCLIENTLIBRARY_GLOBAL_H

#include <QtCore/qglobal.h>

#if defined(KPXCCLIENT_LIBRARY)
#  define KPXCCLIENT_EXPORT Q_DECL_EXPORT
#else
#  define KPXCCLIENT_EXPORT Q_DECL_IMPORT
#endif

#endif // KEEPASSXCCLIENTLIBRARY_GLOBAL_H