/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <QWidget>
#include <QColor>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>

/**
 * @brief A custom color picker widget that displays a color swatch
 *
 * Shows a colored rectangle with a border that opens a color dialog when clicked.
 * Much more visually appealing than a text button.
 */
class ColorPickerWidget : public QWidget
{
    Q_OBJECT

public:
    explicit ColorPickerWidget( const QColor &initialColor = Qt::white, QWidget *parent = nullptr )
        : QWidget( parent )
        , m_color( initialColor )
    {
        setMinimumSize( 60, 24 );
        setMaximumSize( 80, 32 );
        setSizePolicy( QSizePolicy::Preferred, QSizePolicy::Fixed );
        setToolTip( "Click to choose color" );
        setCursor( Qt::PointingHandCursor );
    }

    void setColor( const QColor &color )
    {
        if ( m_color != color )
        {
            m_color = color;
            update(); // Trigger repaint
            emit colorChanged( color );
        }
    }

    QColor color() const
    {
        return m_color;
    }

signals:
    void colorChanged( const QColor &color );
    void clicked();

protected:
    void paintEvent( QPaintEvent *event ) override
    {
        Q_UNUSED( event );

        QPainter painter( this );
        painter.setRenderHint( QPainter::Antialiasing );

        // Draw border
        QPen borderPen( palette().color( QPalette::Text ), 1 );
        painter.setPen( borderPen );

        // Draw filled rectangle with the color
        painter.setBrush( QBrush( m_color ) );
        painter.drawRect( rect().adjusted( 1, 1, -1, -1 ) );
    }

    void mousePressEvent( QMouseEvent *event ) override
    {
        if ( event->button() == Qt::LeftButton )
        {
            emit clicked();
        }
        QWidget::mousePressEvent( event );
    }

    QSize sizeHint() const override
    {
        return QSize( 70, 24 );
    }

private:
    QColor m_color;
};