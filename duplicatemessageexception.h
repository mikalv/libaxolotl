#ifndef DUPLICATEMESSAGEEXCEPTION_H
#define DUPLICATEMESSAGEEXCEPTION_H

#include "whisperexception.h"

class DuplicateMessageException : public WhisperException
{
public:
    DuplicateMessageException(const QString &error): WhisperException("DuplicateMessageException", error) {}
};

#endif // DUPLICATEMESSAGEEXCEPTION_H
