#ifndef NOSESSIONEXCEPTION_H
#define NOSESSIONEXCEPTION_H

#include "whisperexception.h"

class NoSessionException : public WhisperException
{
public:
    NoSessionException(const QString &error) : WhisperException("NoSessionException", error) {}
};

#endif // NOSESSIONEXCEPTION_H
