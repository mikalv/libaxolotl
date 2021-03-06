#include "sessionbuilder.h"

#include "untrustedidentityexception.h"
#include "invalidkeyidexception.h"
#include "invalidkeyexception.h"
#include "invalidmessageexception.h"
#include "stalekeyexchangeexception.h"

#include "ratchet/bobaxolotlparameters.h"
#include "ratchet/ratchetingsession.h"
#include "util/medium.h"
#include "util/keyhelper.h"

#include <QtMath>
#include <QDebug>

SessionBuilder::SessionBuilder()
{

}

SessionBuilder::SessionBuilder(QSharedPointer<SessionStore> sessionStore, QSharedPointer<PreKeyStore> preKeyStore, QSharedPointer<SignedPreKeyStore> signedPreKeyStore, QSharedPointer<IdentityKeyStore> identityKeyStore, const AxolotlAddress &remoteAddress)
{
    init(sessionStore, preKeyStore, signedPreKeyStore, identityKeyStore, remoteAddress);
}

SessionBuilder::SessionBuilder(QSharedPointer<AxolotlStore> store, const AxolotlAddress &remoteAddress)
{
    init(qSharedPointerCast<SessionStore>(store),
         qSharedPointerCast<PreKeyStore>(store),
         qSharedPointerCast<SignedPreKeyStore>(store),
         qSharedPointerCast<IdentityKeyStore>(store),
         remoteAddress);
}

void SessionBuilder::init(QSharedPointer<SessionStore> sessionStore, QSharedPointer<PreKeyStore> preKeyStore, QSharedPointer<SignedPreKeyStore> signedPreKeyStore, QSharedPointer<IdentityKeyStore> identityKeyStore, const AxolotlAddress &remoteAddress)
{
    this->sessionStore      = sessionStore;
    this->preKeyStore       = preKeyStore;
    this->signedPreKeyStore = signedPreKeyStore;
    this->identityKeyStore  = identityKeyStore;
    this->remoteAddress     = remoteAddress;
}

ulong SessionBuilder::process(SessionRecord *sessionRecord, QSharedPointer<PreKeyWhisperMessage> message)
{
    int         messageVersion   = message->getMessageVersion();
    IdentityKey theirIdentityKey = message->getIdentityKey();

    ulong unsignedPreKeyId;

    if (!identityKeyStore->isTrustedIdentity(remoteAddress.getName(), theirIdentityKey)) {
        throw UntrustedIdentityException(QString("Untrusted identity: %1").arg(remoteAddress.getName()));
    }

    qDebug() << messageVersion;
    switch (messageVersion) {
        case 2:  unsignedPreKeyId = processV2(sessionRecord, message); break;
        case 3:  unsignedPreKeyId = processV3(sessionRecord, message); break;
        default: throw InvalidMessageException("Unknown version: " + messageVersion);
    }

    identityKeyStore->saveIdentity(remoteAddress.getName(), theirIdentityKey);
    return unsignedPreKeyId;
}

ulong SessionBuilder::processV3(SessionRecord *sessionRecord, QSharedPointer<PreKeyWhisperMessage> message)
{
    if (sessionRecord->hasSessionState(message->getMessageVersion(), message->getBaseKey().serialize())) {
        return -1;
    }

    ECKeyPair ourSignedPreKey = signedPreKeyStore->loadSignedPreKey(message->getSignedPreKeyId()).getKeyPair();

    BobAxolotlParameters parameters;

    parameters.setTheirBaseKey(message->getBaseKey());
    parameters.setTheirIdentityKey(message->getIdentityKey());
    parameters.setOurIdentityKey(identityKeyStore->getIdentityKeyPair());
    parameters.setOurSignedPreKey(ourSignedPreKey);
    parameters.setOurRatchetKey(ourSignedPreKey);

    if (message->getPreKeyId() >= 0) {
        parameters.setOurOneTimePreKey(preKeyStore->loadPreKey(message->getPreKeyId()).getKeyPair());
    } else {
        //parameters.setOurOneTimePreKey(NULL);
    }

    if (!sessionRecord->isFresh()) sessionRecord->archiveCurrentState();

    RatchetingSession::initializeSession(sessionRecord->getSessionState(), message->getMessageVersion(), parameters);

    sessionRecord->getSessionState()->setLocalRegistrationId(identityKeyStore->getLocalRegistrationId());
    sessionRecord->getSessionState()->setRemoteRegistrationId(message->getRegistrationId());
    sessionRecord->getSessionState()->setAliceBaseKey(message->getBaseKey().serialize());

    if (message->getPreKeyId() != Medium::MAX_VALUE) {
        return message->getPreKeyId();
    } else {
        return -1;
    }
}

ulong SessionBuilder::processV2(SessionRecord *sessionRecord, QSharedPointer<PreKeyWhisperMessage> message)
{
    if (message->getPreKeyId() < 0) {
        throw InvalidKeyIdException("V2 message requires one time prekey id!");
    }

    if (!preKeyStore->containsPreKey(message->getPreKeyId()) &&
        sessionStore->containsSession(remoteAddress))
    {
        return -1;
    }

    ECKeyPair ourPreKey = preKeyStore->loadPreKey(message->getPreKeyId()).getKeyPair();

    BobAxolotlParameters parameters;

    parameters.setOurIdentityKey(identityKeyStore->getIdentityKeyPair());
    parameters.setOurSignedPreKey(ourPreKey);
    parameters.setOurRatchetKey(ourPreKey);
    //parameters.setOurOneTimePreKey(NULL);
    parameters.setTheirIdentityKey(message->getIdentityKey());
    parameters.setTheirBaseKey(message->getBaseKey());

    if (!sessionRecord->isFresh()) sessionRecord->archiveCurrentState();

    RatchetingSession::initializeSession(sessionRecord->getSessionState(), message->getMessageVersion(), parameters);

    sessionRecord->getSessionState()->setLocalRegistrationId(identityKeyStore->getLocalRegistrationId());
    sessionRecord->getSessionState()->setRemoteRegistrationId(message->getRegistrationId());
    sessionRecord->getSessionState()->setAliceBaseKey(message->getBaseKey().serialize());

    if (message->getPreKeyId() != Medium::MAX_VALUE) {
        return message->getPreKeyId();
    } else {
        return -1;
    }
}

void SessionBuilder::process(const PreKeyBundle &preKey)
{
    if (!identityKeyStore->isTrustedIdentity(remoteAddress.getName(), preKey.getIdentityKey())) {
        throw UntrustedIdentityException(QString("Untrusted identity: %1").arg(remoteAddress.getName()));
    }

    if (!preKey.getSignedPreKey().serialize().isEmpty() &&
        !Curve::verifySignature(preKey.getSignedPreKey(),
                                preKey.getIdentityKey().getPublicKey().serialize(),
                                preKey.getSignedPreKeySignature()))
    {
        qWarning() << preKey.getIdentityKey().getPublicKey().serialize().toHex();
        qWarning() << preKey.getSignedPreKey().serialize().toHex();
        qWarning() << preKey.getSignedPreKeySignature().toHex();
        throw InvalidKeyException("Invalid signature on device key!");
    }

    if (preKey.getSignedPreKey().serialize().isEmpty() && preKey.getPreKey().serialize().isEmpty()) {
        throw InvalidKeyException("Both signed and unsigned prekeys are absent!");
    }

    bool           supportsV3           = !preKey.getSignedPreKey().serialize().isEmpty();
    SessionRecord *sessionRecord        = sessionStore->loadSession(remoteAddress);
    ECKeyPair      ourBaseKey           = Curve::generateKeyPair();
    DjbECPublicKey theirSignedPreKey    = supportsV3 ? preKey.getSignedPreKey() : preKey.getPreKey();
    DjbECPublicKey theirOneTimePreKey   = preKey.getPreKey();
    int            theirOneTimePreKeyId = theirOneTimePreKey.serialize().isEmpty() ? -1 : preKey.getPreKeyId();

    AliceAxolotlParameters parameters;

    parameters.setOurBaseKey(ourBaseKey);
    parameters.setOurIdentityKey(identityKeyStore->getIdentityKeyPair());
    parameters.setTheirIdentityKey(preKey.getIdentityKey());
    parameters.setTheirSignedPreKey(theirSignedPreKey);
    parameters.setTheirRatchetKey(theirSignedPreKey);
    if (supportsV3) {
        parameters.setTheirOneTimePreKey(theirOneTimePreKey);
    }

    if (!sessionRecord->isFresh()) sessionRecord->archiveCurrentState();

    RatchetingSession::initializeSession(sessionRecord->getSessionState(),
                                         supportsV3 ? 3 : 2,
                                         parameters);

    sessionRecord->getSessionState()->setUnacknowledgedPreKeyMessage(theirOneTimePreKeyId, preKey.getSignedPreKeyId(), ourBaseKey.getPublicKey());
    sessionRecord->getSessionState()->setLocalRegistrationId(identityKeyStore->getLocalRegistrationId());
    sessionRecord->getSessionState()->setRemoteRegistrationId(preKey.getRegistrationId());
    sessionRecord->getSessionState()->setAliceBaseKey(ourBaseKey.getPublicKey().serialize());

    sessionStore->storeSession(remoteAddress, sessionRecord);
    identityKeyStore->saveIdentity(remoteAddress.getName(), preKey.getIdentityKey());
}

KeyExchangeMessage SessionBuilder::process(QSharedPointer<KeyExchangeMessage> message)
{
    if (!identityKeyStore->isTrustedIdentity(remoteAddress.getName(), message->getIdentityKey())) {
        throw UntrustedIdentityException(QString("Untrusted identity: %1").arg(remoteAddress.getName()));
    }

    KeyExchangeMessage responseMessage;

    if (message->isInitiate()) responseMessage = processInitiate(message);
    else                       processResponse(message);

    return responseMessage;
}

KeyExchangeMessage SessionBuilder::process()
{
    int             sequence         = KeyHelper::getRandomFFFF();
    int             flags            = KeyExchangeMessage::INITIATE_FLAG;
    ECKeyPair       baseKey          = Curve::generateKeyPair();
    ECKeyPair       ratchetKey       = Curve::generateKeyPair();
    IdentityKeyPair identityKey      = identityKeyStore->getIdentityKeyPair();
    QByteArray      baseKeySignature = Curve::calculateSignature(identityKey.getPrivateKey(), baseKey.getPublicKey().serialize());
    SessionRecord  *sessionRecord    = sessionStore->loadSession(remoteAddress);

    sessionRecord->getSessionState()->setPendingKeyExchange(sequence, baseKey, ratchetKey, identityKey);
    sessionStore->storeSession(remoteAddress, sessionRecord);

    return KeyExchangeMessage(2, sequence, flags, baseKey.getPublicKey(), baseKeySignature,
                              ratchetKey.getPublicKey(), identityKey.getPublicKey());
}

KeyExchangeMessage SessionBuilder::processInitiate(QSharedPointer<KeyExchangeMessage> message)
{
    int            flags         = KeyExchangeMessage::RESPONSE_FLAG;
    SessionRecord *sessionRecord = sessionStore->loadSession(remoteAddress);

    if (message->getVersion() >= 3 &&
        !Curve::verifySignature(message->getIdentityKey().getPublicKey(),
                                message->getBaseKey().serialize(),
                                message->getBaseKeySignature()))
    {
        throw InvalidKeyException("Bad signature!");
    }

    SymmetricAxolotlParameters parameters;

    if (!sessionRecord->getSessionState()->hasPendingKeyExchange()) {
        parameters.setOurIdentityKey(identityKeyStore->getIdentityKeyPair());
        parameters.setOurBaseKey(Curve::generateKeyPair());
        parameters.setOurRatchetKey(Curve::generateKeyPair());
    } else {
        parameters.setOurIdentityKey(sessionRecord->getSessionState()->getPendingKeyExchangeIdentityKey());
        parameters.setOurBaseKey(sessionRecord->getSessionState()->getPendingKeyExchangeBaseKey());
        parameters.setOurRatchetKey(sessionRecord->getSessionState()->getPendingKeyExchangeRatchetKey());
        flags |= KeyExchangeMessage::SIMULTAENOUS_INITIATE_FLAG;
    }

    parameters.setTheirBaseKey(message->getBaseKey());
    parameters.setTheirRatchetKey(message->getRatchetKey());
    parameters.setTheirIdentityKey(message->getIdentityKey());

    if (!sessionRecord->isFresh()) sessionRecord->archiveCurrentState();

    RatchetingSession::initializeSession(sessionRecord->getSessionState(),
                                         qMin(message->getMaxVersion(), CiphertextMessage::CURRENT_VERSION),
                                         parameters);

    sessionStore->storeSession(remoteAddress, sessionRecord);
    identityKeyStore->saveIdentity(remoteAddress.getName(), message->getIdentityKey());

    QByteArray baseKeySignature = Curve::calculateSignature(parameters.getOurIdentityKey().getPrivateKey(),
                                                            parameters.getOurBaseKey().getPublicKey().serialize());

    return KeyExchangeMessage(sessionRecord->getSessionState()->getSessionVersion(),
                              message->getSequence(), flags,
                              parameters.getOurBaseKey().getPublicKey(),
                              baseKeySignature, parameters.getOurRatchetKey().getPublicKey(),
                              parameters.getOurIdentityKey().getPublicKey());
}

void SessionBuilder::processResponse(QSharedPointer<KeyExchangeMessage> message)
{
    SessionRecord *sessionRecord                  = sessionStore->loadSession(remoteAddress);
    SessionState  *sessionState                   = sessionRecord->getSessionState();
    bool           hasPendingKeyExchange          = sessionState->hasPendingKeyExchange();
    bool           isSimultaneousInitiateResponse = message->isResponseForSimultaneousInitiate();

    if (!hasPendingKeyExchange || sessionState->getPendingKeyExchangeSequence() != message->getSequence()) {
        if (!isSimultaneousInitiateResponse) throw StaleKeyExchangeException("");
        else                                 return;
    }

    SymmetricAxolotlParameters parameters;

    parameters.setOurBaseKey(sessionRecord->getSessionState()->getPendingKeyExchangeBaseKey());
    parameters.setOurRatchetKey(sessionRecord->getSessionState()->getPendingKeyExchangeRatchetKey());
    parameters.setOurIdentityKey(sessionRecord->getSessionState()->getPendingKeyExchangeIdentityKey());
    parameters.setTheirBaseKey(message->getBaseKey());
    parameters.setTheirRatchetKey(message->getRatchetKey());
    parameters.setTheirIdentityKey(message->getIdentityKey());

    if (!sessionRecord->isFresh()) sessionRecord->archiveCurrentState();

    RatchetingSession::initializeSession(sessionRecord->getSessionState(),
                                         qMin(message->getMaxVersion(), CiphertextMessage::CURRENT_VERSION),
                                         parameters);

    if (sessionRecord->getSessionState()->getSessionVersion() >= 3 &&
        !Curve::verifySignature(message->getIdentityKey().getPublicKey(),
                                message->getBaseKey().serialize(),
                                message->getBaseKeySignature()))
    {
        throw InvalidKeyException("Base key signature doesn't match!");
    }

    sessionStore->storeSession(remoteAddress, sessionRecord);
    identityKeyStore->saveIdentity(remoteAddress.getName(), message->getIdentityKey());
}
