#pragma once
#include <SFML/Graphics.hpp>
#include "../sim/Network.hpp"

struct NodeVisual 
{
    int deviceId;
    sf::Vector2f position;
};

class Renderer 
{
public:
    Renderer(sf::RenderWindow& window, Network& network);

    void updateLayout();
    void draw();

private:
    const NodeVisual* findNodeVisual(int deviceId) const;

    sf::RenderWindow& window_;
    Network& network_;
    std::vector<NodeVisual> visuals_;
};
