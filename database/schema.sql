-- Enable foreign keys on every connection before starting a transaction.
PRAGMA foreign_keys = ON;

BEGIN TRANSACTION;

-- Store registered accounts; password holds the encoded password hash.
CREATE TABLE IF NOT EXISTS users (
    userId INTEGER PRIMARY KEY,
    username TEXT NOT NULL UNIQUE CHECK (length(trim(username)) > 0),
    password TEXT NOT NULL CHECK (length(password) > 0)
);

-- Each event belongs to one registered user.
CREATE TABLE IF NOT EXISTS events (
    eventId INTEGER PRIMARY KEY,
    ownerId INTEGER NOT NULL REFERENCES users(userId),
    title TEXT NOT NULL CHECK (length(trim(title)) > 0),
    startAt TEXT NOT NULL CHECK (length(trim(startAt)) > 0),
    location TEXT NOT NULL CHECK (length(trim(location)) > 0),
    visibility TEXT NOT NULL CHECK (visibility IN ('PUBLIC', 'PRIVATE')),
    description TEXT NOT NULL DEFAULT ''
);

CREATE TABLE IF NOT EXISTS friend_requests (
    friendRequestId INTEGER PRIMARY KEY,
    senderId INTEGER NOT NULL REFERENCES users(userId),
    receiverId INTEGER NOT NULL REFERENCES users(userId),
    status TEXT NOT NULL DEFAULT 'PENDING'
        CHECK (status IN ('PENDING', 'ACCEPTED', 'REJECTED')),
    CHECK (senderId <> receiverId)
);

-- Store each friendship once, with the smaller userId first.
CREATE TABLE IF NOT EXISTS friendships (
    userId1 INTEGER NOT NULL REFERENCES users(userId),
    userId2 INTEGER NOT NULL REFERENCES users(userId),
    PRIMARY KEY (userId1, userId2),
    CHECK (userId1 < userId2)
);

-- Delete event invitations when their event is deleted.
CREATE TABLE IF NOT EXISTS event_invitations (
    invitationId INTEGER PRIMARY KEY,
    eventId INTEGER NOT NULL REFERENCES events(eventId) ON DELETE CASCADE,
    senderId INTEGER NOT NULL REFERENCES users(userId),
    receiverId INTEGER NOT NULL REFERENCES users(userId),
    status TEXT NOT NULL DEFAULT 'PENDING'
        CHECK (status IN ('PENDING', 'ACCEPTED', 'REJECTED'))
);

CREATE TABLE IF NOT EXISTS join_requests (
    joinRequestId INTEGER PRIMARY KEY,
    eventId INTEGER NOT NULL REFERENCES events(eventId) ON DELETE CASCADE,
    userId INTEGER NOT NULL REFERENCES users(userId),
    status TEXT NOT NULL DEFAULT 'PENDING'
        CHECK (status IN ('PENDING', 'ACCEPTED', 'REJECTED'))
);

CREATE TABLE IF NOT EXISTS event_participants (
    eventId INTEGER NOT NULL REFERENCES events(eventId) ON DELETE CASCADE,
    userId INTEGER NOT NULL REFERENCES users(userId),
    PRIMARY KEY (eventId, userId)
);

-- Reject duplicate pending friend requests, including the opposite direction.
CREATE UNIQUE INDEX IF NOT EXISTS unique_pending_friend_request
    ON friend_requests (min(senderId, receiverId), max(senderId, receiverId))
    WHERE status = 'PENDING';

-- Keep past results without allowing another pending invitation for the same user.
CREATE UNIQUE INDEX IF NOT EXISTS unique_pending_event_invitation
    ON event_invitations (eventId, receiverId)
    WHERE status = 'PENDING';

CREATE UNIQUE INDEX IF NOT EXISTS unique_pending_join_request
    ON join_requests (eventId, userId)
    WHERE status = 'PENDING';

-- Support listing owned events, incoming requests, and friendships.
CREATE INDEX IF NOT EXISTS events_by_owner ON events (ownerId);
CREATE INDEX IF NOT EXISTS friend_requests_by_receiver
    ON friend_requests (receiverId, status);
CREATE INDEX IF NOT EXISTS event_invitations_by_receiver
    ON event_invitations (receiverId, status);
CREATE INDEX IF NOT EXISTS friendships_by_second_user ON friendships (userId2);

COMMIT;
