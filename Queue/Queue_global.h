#ifndef QUEUE_GLOBAL_H
#define QUEUE_GLOBAL_H

#include <QtCore/qglobal.h>

#if defined(QUEUE_LIBRARY)
#  define QUEUE_EXPORT Q_DECL_EXPORT
#else
#  define QUEUE_EXPORT Q_DECL_IMPORT
#endif

#endif // QUEUE_GLOBAL_H
