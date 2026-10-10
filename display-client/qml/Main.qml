pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Window
import QtMultimedia

Window {
    id: window
    visible: false
    color: "#000000"
    flags: Qt.Window | Qt.FramelessWindowHint
    title: "Signage Display"

    readonly property bool dashboardMode:
        displayController.displayMode === "dashboard"

    AudioOutput {
        id: audioOutput
        muted: displayController.currentMediaMuted
        volume: displayController.currentMediaVolume
    }

    MediaPlayer {
        id: mediaPlayer
        objectName: "mediaPlayer"
        audioOutput: audioOutput
        videoOutput: videoOutput

        onMediaStatusChanged: {
            if (mediaStatus === MediaPlayer.EndOfMedia)
                displayController.mediaEnded()
        }
        onErrorOccurred: function(error, errorString) {
            displayController.mediaFailed(errorString)
        }
        onPlaybackStateChanged: {
            if (playbackState === MediaPlayer.PlayingState)
                displayController.mediaPlaying()
        }
    }

    Connections {
        target: displayController

        function onPlaybackChanged() {
            mediaPlayer.stop()
            mediaPlayer.source = displayController.currentMediaSource
            if (
                !displaySmokeTest
                && displayController.currentMediaSource.toString().length > 0
            )
                mediaPlayer.play()
        }
    }

    Rectangle {
        anchors.fill: parent
        color: "#000000"

        Item {
            id: safeArea
            x: window.dashboardMode
                ? parent.width * displayController.safeLeft
                : 0
            y: window.dashboardMode
                ? parent.height * displayController.safeTop
                : 0
            width: Math.max(
                0,
                window.dashboardMode
                    ? parent.width * (
                        1
                        - displayController.safeLeft
                        - displayController.safeRight
                    )
                    : parent.width
            )
            height: Math.max(
                0,
                window.dashboardMode
                    ? parent.height * (
                        1
                        - displayController.safeTop
                        - displayController.safeBottom
                    )
                    : parent.height
            )
            clip: true

            Item {
                id: designCanvas
                visible: window.dashboardMode
                anchors.centerIn: parent
                width: 2560
                height: 1440
                scale: Math.min(safeArea.width / width, safeArea.height / height)

                Image {
                    id: originalDesktopSvg
                    objectName: "originalDesktopSvg"
                    anchors.fill: parent
                    source: "qrc:/assets/Desktop.svg"
                    fillMode: Image.Stretch
                    asynchronous: false
                    cache: true
                }

                Item {
                    id: subtitleBand
                    x: 39
                    y: 51
                    width: 1920
                    height: 208
                    clip: true

                    Item {
                        id: subtitleMotion
                        width: parent.width
                        height: parent.height
                        opacity: 0
                        transformOrigin: Item.Center

                        Text {
                            anchors {
                                fill: parent
                                leftMargin: 20
                                rightMargin: 20
                            }
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                            text: displayController.subtitleRichText
                            textFormat: Text.RichText
                            color: "#f7f7f7"
                            font {
                                family: displayFontFamily
                                pixelSize: displayController.subtitleFontSize
                                weight: Font.DemiBold
                            }
                            wrapMode: Text.WordWrap
                            maximumLineCount: 2
                        }
                    }

                    ParallelAnimation {
                        id: subtitleReveal

                        NumberAnimation {
                            target: subtitleMotion
                            property: "opacity"
                            to: 1
                            duration: displayController.subtitleTransitionMs
                            easing.type: Easing.OutCubic
                        }
                        NumberAnimation {
                            target: subtitleMotion
                            property: "x"
                            to: 0
                            duration: displayController.subtitleTransitionMs
                            easing.type: Easing.OutCubic
                        }
                        NumberAnimation {
                            target: subtitleMotion
                            property: "y"
                            to: 0
                            duration: displayController.subtitleTransitionMs
                            easing.type: Easing.OutCubic
                        }
                        NumberAnimation {
                            target: subtitleMotion
                            property: "scale"
                            to: 1
                            duration: displayController.subtitleTransitionMs
                            easing.type: Easing.OutCubic
                        }
                    }

                    function revealSubtitle() {
                        subtitleReveal.stop()
                        subtitleMotion.opacity = 0
                        subtitleMotion.x =
                            displayController.subtitleEffect === "slide_left"
                            ? 130 : 0
                        subtitleMotion.y =
                            displayController.subtitleEffect === "slide_up"
                            ? 48 : 0
                        subtitleMotion.scale =
                            displayController.subtitleEffect === "soft_zoom"
                            ? 0.94 : 1
                        subtitleReveal.start()
                    }

                    Connections {
                        target: displayController
                        function onSubtitleChanged() {
                            subtitleBand.revealSubtitle()
                        }
                    }

                    Component.onCompleted: revealSubtitle()
                }

                Item {
                    id: clockPanel
                    x: 2015
                    y: 51
                    width: 470
                    height: 208
                    // Pi's scene graph can briefly expose a partially rebuilt
                    // glyph atlas when the seconds change over live video.
                    // Composite this small panel as one texture instead.
                    layer.enabled: true
                    layer.smooth: true

                    Rectangle {
                        anchors.fill: parent
                        radius: 18
                        color: "#090d12"
                        border {
                            width: 1
                            color: "#242a31"
                        }
                    }

                    Text {
                        id: clockTimeValue
                        anchors {
                            top: parent.top
                            horizontalCenter: parent.horizontalCenter
                            topMargin: 22
                        }
                        text: displayController.clockTimeText
                        color: "#ffffff"
                        font {
                            family: displayFontFamily
                            pixelSize: 67
                            weight: Font.Bold
                            letterSpacing: -2
                        }
                    }

                    Rectangle {
                        anchors {
                            right: clockTimeValue.left
                            rightMargin: 12
                            verticalCenter: clockTimeValue.verticalCenter
                        }
                        width: 61
                        height: 38
                        radius: 19
                        color: "#202833"

                        Text {
                            anchors.centerIn: parent
                            text: displayController.clockPeriodText
                            color: "#bec8d3"
                            font {
                                family: displayFontFamily
                                pixelSize: 21
                                weight: Font.DemiBold
                            }
                        }
                    }

                    Text {
                        anchors {
                            bottom: parent.bottom
                            horizontalCenter: parent.horizontalCenter
                            bottomMargin: 26
                        }
                        text: displayController.clockDateText
                        color: "#9aa6b2"
                        font {
                            family: displayFontFamily
                            pixelSize: 27
                            weight: Font.Medium
                        }
                    }
                }

                Item {
                    id: dashboardVideoHost
                    x: 40
                    y: 310
                    width: 1920
                    height: 1080
                    clip: true
                }

                Repeater {
                    id: scheduleList
                    objectName: "scheduleList"
                    model: displayController.scheduleModel

                    delegate: Item {
                        id: floorCard
                        required property int floor
                        required property string title
                        required property string location
                        required property string statusCode
                        required property string statusLabel
                        required property string timeRange
                        required property bool hasEvent

                        property real pulseOpacity: 1
                        property color stateColor: {
                            if (statusCode === "active")
                                return "#8e1322"
                            if (statusCode === "starting_soon")
                                return "#76111e"
                            if (statusCode === "upcoming")
                                return "#5b431e"
                            if (statusCode === "inactive")
                                return "#252b32"
                            if (statusCode === "ended")
                                return "#3d4044"
                            return "transparent"
                        }
                        property color accentColor: {
                            if (statusCode === "active")
                                return "#ff4055"
                            if (statusCode === "starting_soon")
                                return "#ff5264"
                            if (statusCode === "upcoming")
                                return "#f2b84b"
                            if (statusCode === "inactive")
                                return "#717b86"
                            if (statusCode === "ended")
                                return "#878b90"
                            return "transparent"
                        }

                        x: 2015
                        y: floor === 4 ? 646 : floor === 3 ? 864 : 1082
                        width: 470
                        height: 180

                        Canvas {
                            id: floorCanvas
                            anchors.fill: parent
                            visible: floorCard.floor === 4 && floorCard.hasEvent
                            opacity: floorCard.pulseOpacity
                            onPaint: {
                                const context = getContext("2d")
                                context.clearRect(0, 0, width, height)
                                context.beginPath()
                                context.moveTo(18.22, 20.16)
                                context.lineTo(30.97, 0)
                                context.lineTo(373.45, 0)
                                context.lineTo(426.28, 26.789)
                                context.lineTo(470, 48.96)
                                context.lineTo(470, 180)
                                context.lineTo(0, 180)
                                context.lineTo(0, 48.96)
                                context.closePath()
                                context.fillStyle = floorCard.stateColor
                                context.fill()
                                context.strokeStyle = floorCard.accentColor
                                context.lineWidth = 4
                                context.stroke()
                            }
                            onVisibleChanged: requestPaint()
                            Connections {
                                target: floorCard
                                function onStateColorChanged() {
                                    floorCanvas.requestPaint()
                                }
                                function onPulseOpacityChanged() {
                                    floorCanvas.requestPaint()
                                }
                            }
                        }

                        Rectangle {
                            anchors.fill: parent
                            visible: floorCard.floor !== 4 && floorCard.hasEvent
                            color: floorCard.stateColor
                            opacity: floorCard.pulseOpacity
                            border {
                                width: 4
                                color: floorCard.accentColor
                            }
                        }

                        Item {
                            visible: floorCard.hasEvent
                            anchors {
                                fill: parent
                                topMargin: floorCard.floor === 4 ? 31 : 18
                                leftMargin: 24
                                rightMargin: 20
                                bottomMargin: 16
                            }
                            opacity: floorCard.statusCode === "ended"
                                ? 0.58
                                : floorCard.statusCode === "inactive" ? 0.72 : 1

                            Text {
                                id: floorTime
                                anchors {
                                    left: parent.left
                                    verticalCenter: parent.verticalCenter
                                }
                                width: 154
                                text: floorCard.timeRange
                                color: floorCard.accentColor
                                font {
                                    family: displayFontFamily
                                    pixelSize: 30
                                    weight: Font.Bold
                                }
                            }

                            Text {
                                anchors {
                                    left: floorTime.right
                                    right: parent.right
                                    verticalCenter: parent.verticalCenter
                                    leftMargin: 10
                                }
                                text: floorCard.title
                                color: "#ffffff"
                                font {
                                    family: displayFontFamily
                                    pixelSize: 36
                                    weight: Font.Bold
                                }
                                elide: Text.ElideRight
                            }

                            Rectangle {
                                anchors {
                                    top: parent.top
                                    right: parent.right
                                }
                                visible: floorCard.statusLabel.length > 0
                                width: statusLabelText.implicitWidth + 22
                                height: 31
                                radius: 15
                                color: floorCard.accentColor

                                Text {
                                    id: statusLabelText
                                    anchors.centerIn: parent
                                    text: floorCard.statusLabel
                                    color: floorCard.statusCode === "upcoming"
                                        ? "#241800" : "#ffffff"
                                    font {
                                        family: displayFontFamily
                                        pixelSize: 17
                                        weight: Font.Bold
                                        letterSpacing: floorCard.statusCode === "active"
                                            ? 1.5 : 0
                                    }
                                }
                            }
                        }

                        SequentialAnimation on pulseOpacity {
                            running:
                                floorCard.statusCode === "starting_soon"
                                && window.visible
                            loops: Animation.Infinite
                            NumberAnimation {
                                to: 0.3
                                duration: 500
                                easing.type: Easing.InOutSine
                            }
                            NumberAnimation {
                                to: 1
                                duration: 500
                                easing.type: Easing.InOutSine
                            }
                        }
                    }
                }

            }

            VideoOutput {
                id: videoOutput
                objectName: "videoOutput"
                parent: window.dashboardMode ? dashboardVideoHost : safeArea
                anchors.fill: parent
                fillMode: displayController.cropMedia
                    ? VideoOutput.PreserveAspectCrop
                    : VideoOutput.PreserveAspectFit
            }

            Text {
                parent: videoOutput
                anchors.centerIn: parent
                width: parent.width * 0.82
                visible: displayController.fallbackMessage.length > 0
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                text: displayController.fallbackMessage
                color: "#d9e5f2"
                font {
                    family: displayFontFamily
                    pixelSize: Math.max(24, parent.height * 0.05)
                    weight: Font.DemiBold
                }
                wrapMode: Text.WordWrap
            }
        }
    }
}
