/*
 Copyright (C) 2010-2014 Kristian Duske
 
 This file is part of TrenchBroom.
 
 TrenchBroom is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.
 
 TrenchBroom is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.
 
 You should have received a copy of the GNU General Public License
 along with TrenchBroom. If not, see <http://www.gnu.org/licenses/>.
 */

#include "BrushRenderer.h"

#include "Preferences.h"
#include "PreferenceManager.h"
#include "Model/Brush.h"
#include "Model/BrushFace.h"
#include "Model/BrushGeometry.h"
#include "Model/EditorContext.h"
#include "Model/NodeVisitor.h"
#include "Renderer/IndexArray.h"
#include "Renderer/RenderContext.h"
#include "Renderer/RenderUtils.h"
#include "Renderer/TexturedIndexArray.h"
#include "Renderer/VertexSpec.h"

namespace TrenchBroom {
    namespace Renderer {
        BrushRenderer::Filter::~Filter() {}
        
        bool BrushRenderer::Filter::show(const Model::BrushFace* face) const      { return doShow(face);  }
        bool BrushRenderer::Filter::show(const Model::BrushEdge* edge) const      { return doShow(edge);  }
        bool BrushRenderer::Filter::transparent(const Model::Brush* brush) const  { return doIsTransparent(brush); }

        BrushRenderer::DefaultFilter::~DefaultFilter() {}
        BrushRenderer::DefaultFilter::DefaultFilter(const Model::EditorContext& context) : m_context(context) {}

        bool BrushRenderer::DefaultFilter::visible(const Model::Brush* brush) const { return m_context.visible(brush); }
        bool BrushRenderer::DefaultFilter::visible(const Model::BrushFace* face) const { return m_context.visible(face); }
        bool BrushRenderer::DefaultFilter::visible(const Model::BrushEdge* edge) const { return m_context.visible(edge->firstFace()->payload()) || m_context.visible(edge->secondFace()->payload()); }
        
        bool BrushRenderer::DefaultFilter::editable(const Model::Brush* brush) const { return m_context.editable(brush); }
        bool BrushRenderer::DefaultFilter::editable(const Model::BrushFace* face) const { return m_context.editable(face); }
        
        bool BrushRenderer::DefaultFilter::selected(const Model::Brush* brush) const { return brush->selected() || brush->parentSelected(); }
        bool BrushRenderer::DefaultFilter::selected(const Model::BrushFace* face) const { return face->selected(); }
        bool BrushRenderer::DefaultFilter::selected(const Model::BrushEdge* edge) const {
            const Model::BrushFace* first = edge->firstFace()->payload();
            const Model::BrushFace* second = edge->secondFace()->payload();
            const Model::Brush* brush = first->brush();
            assert(second->brush() == brush);
            return selected(brush) || selected(first) || selected(second);
        }
        bool BrushRenderer::DefaultFilter::hasSelectedFaces(const Model::Brush* brush) const { return brush->descendantSelected(); }

        BrushRenderer::NoFilter::NoFilter(const bool transparent) : m_transparent(transparent) {}

        bool BrushRenderer::NoFilter::doShow(const Model::BrushFace* face) const { return true; }
        bool BrushRenderer::NoFilter::doShow(const Model::BrushEdge* edge) const { return true; }
        bool BrushRenderer::NoFilter::doIsTransparent(const Model::Brush* brush) const { return m_transparent; }

        BrushRenderer::BrushRenderer(const bool transparent) :
        m_filter(new NoFilter(transparent)),
        m_verticesValid(true),
        m_indicesValid(true),
        m_showEdges(true),
        m_grayscale(false),
        m_tint(false),
        m_showOccludedEdges(false),
        m_transparencyAlpha(1.0f),
        m_showHiddenBrushes(false) {}
        
        BrushRenderer::~BrushRenderer() {
            delete m_filter;
            m_filter = NULL;
        }

        void BrushRenderer::addBrushes(const Model::BrushList& brushes) {
            VectorUtils::append(m_brushes, brushes);
            invalidateIndices();
        }

        void BrushRenderer::setBrushes(const Model::BrushList& brushes) {
            m_brushes = brushes;
            invalidateIndices();
        }

        void BrushRenderer::invalidateVertices() {
            invalidateIndices();
            m_vertexArray = VertexArray();
            m_verticesValid = false;
        }
        
        void BrushRenderer::clear() {
            m_brushes.clear();
            invalidateVertices();
            m_transparentFaceRenderer = FaceRenderer();
            m_opaqueFaceRenderer = FaceRenderer();
        }

        void BrushRenderer::setFaceColor(const Color& faceColor) {
            m_faceColor = faceColor;
        }
        
        void BrushRenderer::setShowEdges(const bool showEdges) {
            m_showEdges = showEdges;
        }
        
        void BrushRenderer::setEdgeColor(const Color& edgeColor) {
            m_edgeColor = edgeColor;
        }
        
        void BrushRenderer::setGrayscale(const bool grayscale) {
            m_grayscale = grayscale;
        }
        
        void BrushRenderer::setTint(const bool tint) {
            m_tint = tint;
        }
        
        void BrushRenderer::setTintColor(const Color& tintColor) {
            m_tintColor = tintColor;
        }

        void BrushRenderer::setShowOccludedEdges(const bool showOccludedEdges) {
            m_showOccludedEdges = showOccludedEdges;
        }
        
        void BrushRenderer::setOccludedEdgeColor(const Color& occludedEdgeColor) {
            m_occludedEdgeColor = occludedEdgeColor;
        }
        
        void BrushRenderer::setTransparencyAlpha(const float transparencyAlpha) {
            m_transparencyAlpha = transparencyAlpha;
        }
        
        void BrushRenderer::setShowHiddenBrushes(const bool showHiddenBrushes) {
            if (showHiddenBrushes == m_showHiddenBrushes)
                return;
            m_showHiddenBrushes = showHiddenBrushes;
            invalidateVertices();
        }

        void BrushRenderer::render(RenderContext& renderContext, RenderBatch& renderBatch) {
            if (!m_brushes.empty()) {
                if (!m_verticesValid)
                    validateVertices();
                if(!m_indicesValid)
                    validateIndices();
                if (renderContext.showFaces())
                    renderFaces(renderBatch);
                if (renderContext.showEdges() && m_showEdges)
                    renderEdges(renderBatch);
            }
        }
        

        void BrushRenderer::renderFaces(RenderBatch& renderBatch) {
            m_opaqueFaceRenderer.setGrayscale(m_grayscale);
            m_opaqueFaceRenderer.setTint(m_tint);
            m_opaqueFaceRenderer.setTintColor(m_tintColor);
            m_opaqueFaceRenderer.render(renderBatch);
            
            m_transparentFaceRenderer.setGrayscale(m_grayscale);
            m_transparentFaceRenderer.setTint(m_tint);
            m_transparentFaceRenderer.setTintColor(m_tintColor);
            m_transparentFaceRenderer.setAlpha(m_transparencyAlpha);
            m_transparentFaceRenderer.render(renderBatch);
        }
        
        void BrushRenderer::renderEdges(RenderBatch& renderBatch) {
            if (m_showOccludedEdges) {
                RenderEdges* renderOccludedEdges = new RenderEdges(Reference::ref(m_edgeRenderer));
                renderOccludedEdges->setRenderOccluded();
                renderOccludedEdges->setColor(m_occludedEdgeColor);
                renderBatch.addOneShot(renderOccludedEdges);
            }
            
            RenderEdges* renderUnoccludedEdges = new RenderEdges(Reference::ref(m_edgeRenderer));
            renderUnoccludedEdges->setColor(m_edgeColor);
            renderBatch.addOneShot(renderUnoccludedEdges);
        }

        class BrushRenderer::FilterWrapper : public BrushRenderer::Filter {
        private:
            const Filter& m_filter;
            bool m_showHiddenBrushes;
        public:
            FilterWrapper(const Filter& filter, const bool showHiddenBrushes) :
            m_filter(filter),
            m_showHiddenBrushes(showHiddenBrushes) {}
            
            bool doShow(const Model::BrushFace* face) const { return m_showHiddenBrushes || m_filter.show(face); }
            bool doShow(const Model::BrushEdge* edge) const { return m_showHiddenBrushes || m_filter.show(edge); }
            bool doIsTransparent(const Model::Brush* brush) const { return m_filter.transparent(brush); }
        };

        class BrushRenderer::CountVertices : public Model::ConstNodeVisitor {
        private:
            const FilterWrapper& m_filter;
            size_t m_vertexCount;
        public:
            CountVertices(const FilterWrapper& filter) :
            m_filter(filter),
            m_vertexCount(0) {}
            
            size_t vertexCount() const {
                return m_vertexCount;
            }
        private:
            void doVisit(const Model::World* world) {}
            void doVisit(const Model::Layer* layer) {}
            void doVisit(const Model::Group* group) {}
            void doVisit(const Model::Entity* entity) {}
            void doVisit(const Model::Brush* brush) {
                countFaceVertices(brush);
            }
            
            void countFaceVertices(const Model::Brush* brush) {
                const Model::BrushFaceList& faces = brush->faces();
                Model::BrushFaceList::const_iterator it, end;
                for (it = faces.begin(), end = faces.end(); it != end; ++it) {
                    const Model::BrushFace* face = *it;
                    if (m_filter.show(face)) {
                        m_vertexCount += face->vertexCount();
                    }
                }
            }
        };

        class BrushRenderer::CollectVertices : public Model::ConstNodeVisitor {
        private:
            const FilterWrapper& m_filter;
            VertexListBuilder<Model::BrushFace::Vertex::Spec> m_builder;
        public:
            CollectVertices(const FilterWrapper& filter, const size_t faceVertexCount) :
            m_filter(filter),
            m_builder(faceVertexCount) {}
            
            VertexArray vertexArray() {
                return VertexArray::swap(m_builder.vertices());
            }
        private:
            void doVisit(const Model::World* world) {}
            void doVisit(const Model::Layer* layer) {}
            void doVisit(const Model::Group* group) {}
            void doVisit(const Model::Entity* entity) {}
            void doVisit(const Model::Brush* brush) {
                collectFaceVertices(brush);
            }
            
            void collectFaceVertices(const Model::Brush* brush) {
                const Model::BrushFaceList& faces = brush->faces();
                Model::BrushFaceList::const_iterator it, end;
                for (it = faces.begin(), end = faces.end(); it != end; ++it) {
                    const Model::BrushFace* face = *it;
                    if (m_filter.show(face))
                        face->getVertices(m_builder);
                }
            }
        };
        
        class BrushRenderer::CountIndices : public Model::ConstNodeVisitor {
        private:
            const FilterWrapper& m_filter;
            TexturedIndexArray::Size m_opaqueIndexSize;
            TexturedIndexArray::Size m_transparentIndexSize;
            IndexArray::Size m_edgeIndexSize;
        public:
            CountIndices(const FilterWrapper& filter) :
            m_filter(filter) {}
            
            const TexturedIndexArray::Size& opaqueIndexSize() const {
                return m_opaqueIndexSize;
            }
            
            const TexturedIndexArray::Size& transparentIndexSize() const {
                return m_transparentIndexSize;
            }
            
            const IndexArray::Size& edgeIndexSize() const {
                return m_edgeIndexSize;
            }
        private:
            void doVisit(const Model::World* world) {}
            void doVisit(const Model::Layer* layer) {}
            void doVisit(const Model::Group* group) {}
            void doVisit(const Model::Entity* entity) {}
            void doVisit(const Model::Brush* brush) {
                countFaceIndices(brush);
            }
            
            void countFaceIndices(const Model::Brush* brush) {
                const Model::BrushFaceList& faces = brush->faces();
                Model::BrushFaceList::const_iterator it, end;
                for (it = faces.begin(), end = faces.end(); it != end; ++it) {
                    const Model::BrushFace* face = *it;
                    if (m_filter.show(face)) {
                        if (m_filter.transparent(brush))
                            m_transparentIndexSize.inc(face->texture(), PT_Polygons);
                        else
                            m_opaqueIndexSize.inc(face->texture(), PT_Polygons);
                        m_edgeIndexSize.inc(PT_LineLoops);
                    }
                }
            }
        };
        
        class BrushRenderer::CollectIndices : public Model::ConstNodeVisitor {
        private:
            const FilterWrapper& m_filter;
            TexturedIndexArray m_opaqueFaceIndices;
            TexturedIndexArray m_transparentFaceIndices;
            IndexArray m_edgeIndices;
        public:
            CollectIndices(const FilterWrapper& filter, const CountIndices& indexSize) :
            m_filter(filter),
            m_opaqueFaceIndices(indexSize.opaqueIndexSize()),
            m_transparentFaceIndices(indexSize.transparentIndexSize()),
            m_edgeIndices(indexSize.edgeIndexSize()) {}
            
            const TexturedIndexArray& opaqueFaceIndices() const {
                return m_opaqueFaceIndices;
            }
            
            const TexturedIndexArray& transparentFaceIndices() const {
                return m_transparentFaceIndices;
            }
            
            const IndexArray& edgeIndices() const {
                return m_edgeIndices;
            }
        private:
            void doVisit(const Model::World* world) {}
            void doVisit(const Model::Layer* layer) {}
            void doVisit(const Model::Group* group) {}
            void doVisit(const Model::Entity* entity) {}
            void doVisit(const Model::Brush* brush) {
                collectFaceIndices(brush);
            }
            
            void collectFaceIndices(const Model::Brush* brush) {
                const Model::BrushFaceList& faces = brush->faces();
                Model::BrushFaceList::const_iterator it, end;
                for (it = faces.begin(), end = faces.end(); it != end; ++it) {
                    const Model::BrushFace* face = *it;
                    if (m_filter.show(face)) {
                        if (m_filter.transparent(brush))
                            face->getFaceIndex(m_transparentFaceIndices);
                        else
                            face->getFaceIndex(m_opaqueFaceIndices);
                        face->getEdgeIndex(m_edgeIndices);
                    }
                }
            }
        };
        
        void BrushRenderer::invalidateIndices() {
            m_indicesValid = false;
        }
        
        void BrushRenderer::validateVertices() {
            assert(!m_verticesValid);
            
            const FilterWrapper wrapper(*m_filter, m_showHiddenBrushes);
            CountVertices countVertices(wrapper);
            Model::Node::accept(m_brushes.begin(), m_brushes.end(), countVertices);
            
            CollectVertices collectVertices(wrapper, countVertices.vertexCount());
            Model::Node::accept(m_brushes.begin(), m_brushes.end(), collectVertices);
            
            m_vertexArray = collectVertices.vertexArray();
            m_verticesValid = true;
        }
        
        void BrushRenderer::validateIndices() {
            assert(!m_indicesValid);
            
            const FilterWrapper wrapper(*m_filter, m_showHiddenBrushes);
            CountIndices countIndices(wrapper);
            Model::Node::accept(m_brushes.begin(), m_brushes.end(), countIndices);
            
            CollectIndices collectIndices(wrapper, countIndices);
            Model::Node::accept(m_brushes.begin(), m_brushes.end(), collectIndices);
            
            m_opaqueFaceRenderer = FaceRenderer(m_vertexArray, collectIndices.opaqueFaceIndices(), m_faceColor);
            m_transparentFaceRenderer = FaceRenderer(m_vertexArray, collectIndices.transparentFaceIndices(), m_faceColor);
            m_edgeRenderer = EdgeRenderer(m_vertexArray, collectIndices.edgeIndices());
            
            m_indicesValid = true;
        }
    }
}
