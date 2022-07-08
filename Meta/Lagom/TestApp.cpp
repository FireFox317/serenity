/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibCore/Timer.h>
#include <LibCore/Directory.h>
#include <LibCore/System.h>
#include <stdio.h>
#include <errno.h>
#include <spawn.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <LibImageDecoderClient/Client.h>
#include <LibMain/Main.h>

extern char** environ;


ErrorOr<int> serenity_main(Main::Arguments) {
    Core::EventLoop event_loop;

    char const* socket_path = "/tmp/portal/image";
    TRY(Core::Directory::create(LexicalPath(socket_path).parent(), Core::Directory::CreateDirectories::Yes));

    // Note: we use SOCK_CLOEXEC here to make sure we don't leak every socket to
    // all the clients. We'll make the one we do need to pass down !CLOEXEC later
    // after forking off the process.
    // int const socket_fd = TRY(Core::System::socket(AF_LOCAL, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    int const socket_fd = TRY(Core::System::socket(AF_LOCAL, SOCK_STREAM, 0));
    // socket.fd = socket_fd;

    // if (m_account.has_value()) {
    //     auto& account = m_account.value();
    //     TRY(Core::System::fchown(socket_fd, account.uid(), account.gid()));
    // }

    // TRY(Core::System::fchmod(socket_fd, socket.permissions));

    auto socket_address = Core::SocketAddress::local(socket_path);
    auto un_optional = socket_address.to_sockaddr_un();
    if (!un_optional.has_value()) {
        dbgln("Socket name {} is too long. BUG! This should have failed earlier!", socket_path);
        VERIFY_NOT_REACHED();
    }
    auto un = un_optional.value();

    TRY(Core::System::bind(socket_fd, (sockaddr const*)&un, sizeof(un)));
    TRY(Core::System::listen(socket_fd, 16));
    

    char const* command = "/Users/timon/serenity/Build/lagom/Services/ImageDecoder/ImageDecoder";

    StringBuilder builder;
    int new_fd = dup(socket_fd);
    builder.appendff("{}:{}", socket_path, new_fd);

    setenv("SOCKET_TAKEOVER", builder.to_string().characters(), true);    
    pid_t child_pid;
    char const* argv[] = { command, nullptr };
    if ((errno = posix_spawn(&child_pid, command, nullptr, nullptr, const_cast<char**>(argv), environ))) {
        perror("posix_spawn");
        exit(1);
    }
    unsetenv("SOCKET_TAKEOVER");

    RefPtr<ImageDecoderClient::Client> client = TRY(ImageDecoderClient::Client::try_create()); 
    client->on_death = [&] {
        client = nullptr;
    };

    auto timer = Core::Timer::construct(100, [&] {
        dbgln("Timer fired, good-bye! :^)");
        // event_loop.quit(0);
    });

    return event_loop.exec();
}
