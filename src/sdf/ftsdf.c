
#include <freetype/internal/ftobjs.h>
#include <freetype/internal/ftdebug.h>
#include <freetype/fttrigon.h>
#include "ftsdf.h"

#include "ftsdferrs.h"

  /**************************************************************************
   *
   * definitions
   *
   */

  /* If it is defined to 1 then the rasterizer will use squared distances */
  /* for computation. It can greatly improve the performance but there is */
  /* a chance of overflow and artifacts. You can safely use it upto a     */
  /* pixel size of 128.                                                   */
  #ifndef USE_SQUARED_DISTANCES
  #  define USE_SQUARED_DISTANCES 0
  #endif

  /* If it is defined to 1 then the rasterizer will use Newton-Raphson's  */
  /* method for finding shortest distance from a point to a conic curve.  */
  /* The other method is an analytical method which find the roots of a   */
  /* cubic polynomial to find the shortest distance. But the analytical   */
  /* method has underflow as of now. So, use the Newton's method if there */
  /* is any visible artifacts.                                            */
  #ifndef USE_NEWTON_FOR_CONIC
  #  define USE_NEWTON_FOR_CONIC 1
  #endif

  /* `MAX_NEWTON_DIVISIONS' is the number of intervals the bezier curve   */
  /* is sampled and checked for shortest distance.                        */
  #define MAX_NEWTON_DIVISIONS   4

  /* `MAX_NEWTON_STEPS' is the number of steps of Newton's iterations in  */
  /* each interval of the bezier curve. Basically for each division we    */
  /* run the Newton's approximation (i.e. x -= Q( t ) / Q'( t )) to get   */
  /* the shortest distance.                                               */
  #define MAX_NEWTON_STEPS       4

  /* This is the distance in 16.16 which is used for corner resolving. If */
  /* the difference of two distance is less than `CORNER_CHECK_EPSILON'   */
  /* then they will be checked for corner if they have ambiguity.         */
  #define CORNER_CHECK_EPSILON   32

  #define CG_DIMEN               8

  /**************************************************************************
   *
   * macros
   *
   */

  /* convert int to 26.6 fixed point   */
  #define FT_INT_26D6( x )   ( x * 64 )

  /* convert int to 16.16 fixed point  */
  #define FT_INT_16D16( x )  ( x * 65536 )

  /* convert 26.6 to 16.16 fixed point */
  #define FT_26D6_16D16( x ) ( x * 1024 )


  /* Convenient macro which calls the function */
  /* and returns if any error occurs.          */
  #define FT_CALL( x ) do                          \
                       {                           \
                         error = ( x );            \
                         if ( error != FT_Err_Ok ) \
                           goto Exit;              \
                       } while ( 0 )

  #define MUL_26D6( a, b ) ( ( a * b ) / 64 )
  #define VEC_26D6_DOT( p, q ) ( MUL_26D6( p.x, q.x ) +   \
                                 MUL_26D6( p.y, q.y ) )

  /* [IMPORTANT]: The macro `VECTOR_LENGTH_16D16' is not always the same */
  /* and must not be used anywhere except a few places. This macro is    */
  /* controlled by the `USE_SQUARED_DISTANCES' macro. It compute squared */
  /* distance or actual distance based on `USE_SQUARED_DISTANCES' value. */
  /* By using squared distances the performance can be greatly improved  */
  /* but there is a risk of overflow. Use it wisely.                     */
  #if USE_SQUARED_DISTANCES
  #  define VECTOR_LENGTH_16D16( v ) ( FT_MulFix( v.x, v.x ) + \
                                       FT_MulFix( v.y, v.y ) )
  #else
  #  define VECTOR_LENGTH_16D16( v ) FT_Vector_Length( &v )
  #endif

  /**************************************************************************
   *
   * typedefs
   *
   */

  typedef  FT_Vector FT_26D6_Vec;   /* with 26.6 fixed point components  */
  typedef  FT_Vector FT_16D16_Vec;  /* with 16.16 fixed point components */

  typedef  FT_Fixed  FT_16D16;      /* 16.16 fixed point representation  */
  typedef  FT_Fixed  FT_26D6;       /* 26.6 fixed point representation   */

  typedef  FT_BBox   FT_CBox;       /* control box of a curve            */

  /**************************************************************************
   *
   * structures and enums
   *
   */

  typedef struct  SDF_TRaster_
  {
    FT_Memory  memory; /* used internally to allocate memory */

  } SDF_TRaster;

  /* enumeration of all the types of curve present in vector fonts */
  typedef enum  SDF_Edge_Type_
  {
    SDF_EDGE_UNDEFINED  = 0,  /* undefined, used to initialize */
    SDF_EDGE_LINE       = 1,  /* straight line segment         */
    SDF_EDGE_CONIC      = 2,  /* second order bezier curve     */
    SDF_EDGE_CUBIC      = 3   /* third order bezier curve      */

  } SDF_Edge_Type;

  /* represent a single edge in a contour */
  typedef struct  SDF_Edge_
  {
    FT_26D6_Vec    start_pos;   /* start position of the edge             */
    FT_26D6_Vec    end_pos;     /* end position of the edge               */
    FT_26D6_Vec    control_a;   /* first control point of a bezier curve  */
    FT_26D6_Vec    control_b;   /* second control point of a bezier curve */

    SDF_Edge_Type  edge_type;   /* edge identifier                        */

    struct SDF_Edge_*   next;   /* to create linked list                  */

  } SDF_Edge;

  /* A contour represent a set of edges which make a closed */
  /* loop.                                                  */
  typedef struct  SDF_Contour_
  {
    FT_26D6_Vec           last_pos;  /* end position of the last edge    */
    SDF_Edge*             edges;     /* list of all edges in the contour */

    struct SDF_Contour_*  next;      /* to create linked list            */

  } SDF_Contour;

  /* Represent a set a contours which makes up a complete */
  /* glyph outline.                                       */
  typedef struct  SDF_Shape_
  {
    FT_Memory     memory;    /* used internally to allocate memory  */
    SDF_Contour*  contours;  /* list of all contours in the outline */

  } SDF_Shape;

  typedef struct SDF_Signed_Distance_
  {
    /* Unsigned shortest distance from the point to   */
    /* the above `nearest_point'.                     */
    /* [NOTE]: This can represent both squared as or  */
    /* actual distance. This is controlled by the     */
    /* `USE_SQUARED_DISTANCES' macro.                 */
    FT_16D16      distance;

    /* The cross product of distance vector and the   */
    /* direction. ( aka orthogonality )               */
    FT_16D16      cross;

    /* Represent weather the distance vector is outside */
    /* or inside the contour corresponding to the edge. */
    /* [note]: This sign may or may not be correct,     */
    /*         therefore it must be checked properly in */
    /*         case there is an ambiguity.              */
    FT_Char       sign;       

  } SDF_Signed_Distance;

  typedef struct SDF_Params_
  {
    /* Used to properly assign sign to the pixels. In  */
    /* truetype the right should be inside but in post */
    /* script left should be inside.                   */
    FT_Orientation  orientation;

    /* Simply flip the sign if this is true.           */
    FT_Bool         flip_sign;

    /* If set to true the output will be upside down.  */
    /* Can be useful because OpenGL and DirectX have   */
    /* different coordinate system for textures.       */
    FT_Bool         flip_y;

  } SDF_Params;

  /**************************************************************************
   *
   * constants, initializer and destructor
   *
   */

  static
  const FT_Vector    zero_vector  = { 0, 0 };

  static
  const SDF_Edge     null_edge    = { { 0, 0 }, { 0, 0 },
                                      { 0, 0 }, { 0, 0 },
                                      SDF_EDGE_UNDEFINED, NULL };

  static
  const SDF_Contour  null_contour = { { 0, 0 }, NULL, NULL };

  static
  const SDF_Shape    null_shape   = { NULL, NULL };

  static
  const SDF_Signed_Distance  max_sdf = { INT_MAX, 0, 0 };

  /* Creates a new `SDF_Edge' on the heap and assigns the `edge' */
  /* pointer to the newly allocated memory.                      */
  static FT_Error
  sdf_edge_new( FT_Memory   memory,
                SDF_Edge**  edge )
  {
    FT_Error   error = FT_Err_Ok;
    SDF_Edge*  ptr   = NULL;


    if ( !memory || !edge )
    {
      error = FT_THROW( Invalid_Argument );
      goto Exit;
    }

    if ( !FT_QNEW( ptr ) )
    {
      *ptr = null_edge;
      *edge = ptr;
    }

  Exit:
    return error;
  }

  /* Frees the allocated `edge' variable. */
  static void
  sdf_edge_done( FT_Memory   memory,
                 SDF_Edge**  edge )
  {
    if ( !memory || !edge || !*edge )
      return;

    FT_FREE( *edge );
  }

  /* Creates a new `SDF_Contour' on the heap and assigns  */
  /* the `contour' pointer to the newly allocated memory. */
  static FT_Error
  sdf_contour_new( FT_Memory      memory,
                   SDF_Contour**  contour )
  {
    FT_Error      error = FT_Err_Ok;
    SDF_Contour*  ptr   = NULL;


    if ( !memory || !contour )
    {
      error = FT_THROW( Invalid_Argument );
      goto Exit;
    }

    if ( !FT_QNEW( ptr ) )
    {
      *ptr = null_contour;
      *contour = ptr;
    }

  Exit:
    return error;
  }

  /* Frees the allocated `contour' variable and also frees */
  /* the list of edges.                                    */
  static void
  sdf_contour_done( FT_Memory      memory,
                    SDF_Contour**  contour )
  {
    SDF_Edge*  edges;
    SDF_Edge*  temp;

    if ( !memory || !contour || !*contour )
      return;

    edges = (*contour)->edges;

    /* release all the edges */
    while ( edges )
    {
      temp = edges;
      edges = edges->next;

      sdf_edge_done( memory, &temp );
    }

    FT_FREE( *contour );
  }

  /* Creates a new `SDF_Shape' on the heap and assigns  */
  /* the `shape' pointer to the newly allocated memory. */
  static FT_Error
  sdf_shape_new( FT_Memory    memory,
                 SDF_Shape**  shape )
  {
    FT_Error      error = FT_Err_Ok;
    SDF_Shape*    ptr   = NULL;


    if ( !memory || !shape )
    {
      error = FT_THROW( Invalid_Argument );
      goto Exit;
    }

    if ( !FT_QNEW( ptr ) )
    {
      *ptr = null_shape;
      ptr->memory = memory;
      *shape = ptr;
    }

  Exit:
    return error;
  }

  /* Frees the allocated `shape' variable and also frees */
  /* the list of contours.                               */
  static void
  sdf_shape_done( SDF_Shape**  shape )
  {
    FT_Memory     memory;
    SDF_Contour*  contours;
    SDF_Contour*  temp;


    if ( !shape || !*shape )
      return;

    memory = (*shape)->memory;
    contours = (*shape)->contours;

    if ( !memory )
      return;

    /* release all the contours */
    while ( contours )
    {
      temp = contours;
      contours = contours->next;

      sdf_contour_done( memory, &temp );
    }

    /* release the allocated shape struct  */
    FT_FREE( *shape );
  }
    

  /**************************************************************************
   *
   * shape decomposition functions
   *
   */

  /* This function is called when walking along a new contour */
  /* so add a new contour to the shape's list.                */
  static FT_Error
  sdf_move_to( const FT_26D6_Vec* to,
               void*              user )
  {
    SDF_Shape*    shape    = ( SDF_Shape* )user;
    SDF_Contour*  contour  = NULL;

    FT_Error      error    = FT_Err_Ok;
    FT_Memory     memory   = shape->memory;


    if ( !to || !user )
    {
      error = FT_THROW( Invalid_Argument );
      goto Exit;
    }

    FT_CALL( sdf_contour_new( memory, &contour ) );

    contour->last_pos = *to;
    contour->next = shape->contours;
    shape->contours = contour;

  Exit:
    return error;
  }

  static FT_Error
  sdf_line_to( const FT_26D6_Vec*  to,
               void*               user )
  {
    SDF_Shape*    shape    = ( SDF_Shape* )user;
    SDF_Edge*     edge     = NULL;
    SDF_Contour*  contour  = NULL;

    FT_Error      error    = FT_Err_Ok;
    FT_Memory     memory   = shape->memory;


    if ( !to || !user )
    {
      error = FT_THROW( Invalid_Argument );
      goto Exit;
    }

    contour = shape->contours;

    if ( contour->last_pos.x == to->x && 
         contour->last_pos.y == to->y )
      goto Exit;

    FT_CALL( sdf_edge_new( memory, &edge ) );

    edge->edge_type = SDF_EDGE_LINE;
    edge->start_pos = contour->last_pos;
    edge->end_pos   = *to;

    edge->next = contour->edges;
    contour->edges = edge;
    contour->last_pos = *to;

  Exit:
    return error;
  }

  static FT_Error
  sdf_conic_to( const FT_26D6_Vec*  control_1,
                const FT_26D6_Vec*  to,
                void*               user )
  {
    SDF_Shape*    shape    = ( SDF_Shape* )user;
    SDF_Edge*     edge     = NULL;
    SDF_Contour*  contour  = NULL;

    FT_Error      error    = FT_Err_Ok;
    FT_Memory     memory   = shape->memory;


    if ( !control_1 || !to || !user )
    {
      error = FT_THROW( Invalid_Argument );
      goto Exit;
    }

    contour = shape->contours;

    FT_CALL( sdf_edge_new( memory, &edge ) );

    edge->edge_type = SDF_EDGE_CONIC;
    edge->start_pos = contour->last_pos;
    edge->control_a = *control_1;
    edge->end_pos   = *to;

    edge->next = contour->edges;
    contour->edges = edge;
    contour->last_pos = *to;

  Exit:
    return error;
  }

  static FT_Error
  sdf_cubic_to( const FT_26D6_Vec*  control_1,
                const FT_26D6_Vec*  control_2,
                const FT_26D6_Vec*  to,
                void*               user )
  {
    SDF_Shape*    shape    = ( SDF_Shape* )user;
    SDF_Edge*     edge     = NULL;
    SDF_Contour*  contour  = NULL;

    FT_Error      error    = FT_Err_Ok;
    FT_Memory     memory   = shape->memory;


    if ( !control_2 || !control_1 || !to || !user )
    {
      error = FT_THROW( Invalid_Argument );
      goto Exit;
    }

    contour = shape->contours;

    FT_CALL( sdf_edge_new( memory, &edge ) );

    edge->edge_type = SDF_EDGE_CUBIC;
    edge->start_pos = contour->last_pos;
    edge->control_a = *control_1;
    edge->control_b = *control_2;
    edge->end_pos   = *to;

    edge->next = contour->edges;
    contour->edges = edge;
    contour->last_pos = *to;

  Exit:
    return error;
  }

  FT_DEFINE_OUTLINE_FUNCS(
    sdf_decompose_funcs,

    (FT_Outline_MoveTo_Func)  sdf_move_to,   /* move_to  */
    (FT_Outline_LineTo_Func)  sdf_line_to,   /* line_to  */
    (FT_Outline_ConicTo_Func) sdf_conic_to,  /* conic_to */
    (FT_Outline_CubicTo_Func) sdf_cubic_to,  /* cubic_to */

    0,                                       /* shift    */
    0                                        /* delta    */
  )

  /* function decomposes the outline and puts it into the `shape' struct */
  static FT_Error
  sdf_outline_decompose( FT_Outline*  outline,
                         SDF_Shape*   shape )
  {
    FT_Error  error = FT_Err_Ok;


    if ( !outline || !shape )
    {
      error = FT_THROW( Invalid_Argument );
      goto Exit;
    }

    error = FT_Outline_Decompose( outline, 
                                  &sdf_decompose_funcs,
                                  (void*)shape );

  Exit:
    return error;
  }

  /**************************************************************************
   *
   * utility functions
   *
   */

  /* The function returns the control box of a edge. */
  /* The control box is a rectangle in which all the */
  /* control points can fit tightly.                 */
  static FT_CBox
  get_control_box( SDF_Edge  edge )
  {
    FT_CBox  cbox;
    FT_Bool  is_set = 0;


    switch (edge.edge_type) {
    case SDF_EDGE_CUBIC:
    {
      cbox.xMin = edge.control_b.x;
      cbox.xMax = edge.control_b.x;
      cbox.yMin = edge.control_b.y;
      cbox.yMax = edge.control_b.y;

      is_set = 1;
    }
    case SDF_EDGE_CONIC:
    {
      if ( is_set )
      {
        cbox.xMin = edge.control_a.x < cbox.xMin ?
                    edge.control_a.x : cbox.xMin;
        cbox.xMax = edge.control_a.x > cbox.xMax ?
                    edge.control_a.x : cbox.xMax;

        cbox.yMin = edge.control_a.y < cbox.yMin ?
                    edge.control_a.y : cbox.yMin;
        cbox.yMax = edge.control_a.y > cbox.yMax ?
                    edge.control_a.y : cbox.yMax;
      }
      else
      {
        cbox.xMin = edge.control_a.x;
        cbox.xMax = edge.control_a.x;
        cbox.yMin = edge.control_a.y;
        cbox.yMax = edge.control_a.y;

        is_set = 1;
      }
    }
    case SDF_EDGE_LINE:
    {
      if ( is_set )
      {
        cbox.xMin = edge.start_pos.x < cbox.xMin ?
                    edge.start_pos.x : cbox.xMin;
        cbox.xMax = edge.start_pos.x > cbox.xMax ?
                    edge.start_pos.x : cbox.xMax;

        cbox.yMin = edge.start_pos.y < cbox.yMin ?
                    edge.start_pos.y : cbox.yMin;
        cbox.yMax = edge.start_pos.y > cbox.yMax ?
                    edge.start_pos.y : cbox.yMax;
      }
      else
      {
        cbox.xMin = edge.start_pos.x;
        cbox.xMax = edge.start_pos.x;
        cbox.yMin = edge.start_pos.y;
        cbox.yMax = edge.start_pos.y;
      }

      cbox.xMin = edge.end_pos.x < cbox.xMin ?
                  edge.end_pos.x : cbox.xMin;
      cbox.xMax = edge.end_pos.x > cbox.xMax ?
                  edge.end_pos.x : cbox.xMax;

      cbox.yMin = edge.end_pos.y < cbox.yMin ?
                  edge.end_pos.y : cbox.yMin;
      cbox.yMax = edge.end_pos.y > cbox.yMax ?
                  edge.end_pos.y : cbox.yMax;

      break;
    }
    default:
        break;
    }

    return cbox;
  }

  /* The function is exactly same as the one    */
  /* in the smooth renderer. It splits a conic  */
  /* into two conic exactly half way at t = 0.5 */
  static void
  split_conic( FT_26D6_Vec*  base )
  {
    FT_26D6  a, b;


    base[4].x = base[2].x;
    a = base[0].x + base[1].x;
    b = base[1].x + base[2].x;
    base[3].x = b / 2;
    base[2].x = ( a + b ) / 4;
    base[1].x = a / 2;

    base[4].y = base[2].y;
    a = base[0].y + base[1].y;
    b = base[1].y + base[2].y;
    base[3].y = b / 2;
    base[2].y = ( a + b ) / 4;
    base[1].y = a / 2;
  }

  /* The function is exactly same as the one    */
  /* in the smooth renderer. It splits a cubic  */
  /* into two cubic exactly half way at t = 0.5 */
  static void
  split_cubic( FT_26D6_Vec*  base )
  {
    FT_26D6  a, b, c;


    base[6].x = base[3].x;
    a = base[0].x + base[1].x;
    b = base[1].x + base[2].x;
    c = base[2].x + base[3].x;
    base[5].x = c / 2;
    c += b;
    base[4].x = c / 4;
    base[1].x = a / 2;
    a += b;
    base[2].x = a / 4;
    base[3].x = ( a + c ) / 8;

    base[6].y = base[3].y;
    a = base[0].y + base[1].y;
    b = base[1].y + base[2].y;
    c = base[2].y + base[3].y;
    base[5].y = c / 2;
    c += b;
    base[4].y = c / 4;
    base[1].y = a / 2;
    a += b;
    base[2].y = a / 4;
    base[3].y = ( a + c ) / 8;
  }

  /* the function splits a conic bezier curve     */
  /* into a number of lines and adds them to      */
  /* a list `out'. The function uses recursion    */
  /* that is why a `max_splits' param is required */
  /* for stopping.                                */
  static FT_Error
  split_sdf_conic( FT_Memory     memory,
                   FT_26D6_Vec*  control_points,
                   FT_Int        max_splits,
                   SDF_Edge**    out )
  {
    FT_Error     error = FT_Err_Ok;
    FT_26D6_Vec  cpos[5];
    SDF_Edge*    left,*  right;


    if ( !memory || !out  )
    {
      error = FT_THROW( Invalid_Argument );
      goto Exit;
    }

    /* split the conic */
    cpos[0] = control_points[0];
    cpos[1] = control_points[1];
    cpos[2] = control_points[2];

    split_conic( cpos );

    /* If max number of splits is done */
    /* then stop and add the lines to  */
    /* the list.                       */
    if ( max_splits <= 2 )
      goto Append;

    /* If not max splits then keep splitting */
    FT_CALL( split_sdf_conic( memory, &cpos[0], max_splits / 2, out ) );
    FT_CALL( split_sdf_conic( memory, &cpos[2], max_splits / 2, out ) );

    /* [NOTE]: This is not an efficient way of   */
    /* splitting the curve. Check the deviation  */
    /* instead and stop if the deviation is less */
    /* than a pixel.                             */

    goto Exit;

  Append:

    /* Allocation and add the lines to the list. */

    FT_CALL( sdf_edge_new( memory, &left) );
    FT_CALL( sdf_edge_new( memory, &right) );

    left->start_pos  = cpos[0];
    left->end_pos    = cpos[2];
    left->edge_type  = SDF_EDGE_LINE;

    right->start_pos = cpos[2];
    right->end_pos   = cpos[4];
    right->edge_type = SDF_EDGE_LINE;

    left->next = right;
    right->next = (*out);
    *out = left;

  Exit:
    return error;
  }

  /* the function splits a cubic bezier curve     */
  /* into a number of lines and adds them to      */
  /* a list `out'. The function uses recursion    */
  /* that is why a `max_splits' param is required */
  /* for stopping.                                */
  static FT_Error
  split_sdf_cubic( FT_Memory     memory,
                   FT_26D6_Vec*  control_points,
                   FT_Int        max_splits,
                   SDF_Edge**    out )
  {
    FT_Error     error = FT_Err_Ok;
    FT_26D6_Vec  cpos[7];
    SDF_Edge*    left,*  right;


    if ( !memory || !out  )
    {
      error = FT_THROW( Invalid_Argument );
      goto Exit;
    }

    /* split the conic */
    cpos[0] = control_points[0];
    cpos[1] = control_points[1];
    cpos[2] = control_points[2];
    cpos[3] = control_points[3];

    split_cubic( cpos );

    /* If max number of splits is done */
    /* then stop and add the lines to  */
    /* the list.                       */
    if ( max_splits <= 2 )
      goto Append;

    /* If not max splits then keep splitting */
    FT_CALL( split_sdf_cubic( memory, &cpos[0], max_splits / 2, out ) );
    FT_CALL( split_sdf_cubic( memory, &cpos[3], max_splits / 2, out ) );

    /* [NOTE]: This is not an efficient way of   */
    /* splitting the curve. Check the deviation  */
    /* instead and stop if the deviation is less */
    /* than a pixel.                             */

    goto Exit;

  Append:

    /* Allocation and add the lines to the list. */

    FT_CALL( sdf_edge_new( memory, &left) );
    FT_CALL( sdf_edge_new( memory, &right) );

    left->start_pos  = cpos[0];
    left->end_pos    = cpos[3];
    left->edge_type  = SDF_EDGE_LINE;

    right->start_pos = cpos[3];
    right->end_pos   = cpos[6];
    right->edge_type = SDF_EDGE_LINE;

    left->next = right;
    right->next = (*out);
    *out = left;

  Exit:
    return error;
  }

  /* This function subdivide and entire shape   */
  /* into line segment such that the it doesn't */
  /* look visually different than the original  */
  /* curve.                                     */
  static FT_Error
  split_sdf_shape( SDF_Shape*  shape )
  {
    FT_Error      error = FT_Err_Ok;
    FT_Memory     memory;

    SDF_Contour*  contours;
    SDF_Contour*  new_contours = NULL;



    if ( !shape || !shape->memory )
    {
      error = FT_THROW( Invalid_Argument );
      goto Exit;
    }

    contours = shape->contours;
    memory = shape->memory;

    /* for each contour */
    while ( contours )
    {
      SDF_Edge*     edges = contours->edges;
      SDF_Edge*     new_edges = NULL;

      SDF_Contour*  tempc;

      /* for each edge */
      while ( edges )
      {
        SDF_Edge*    edge = edges;
        SDF_Edge*    temp;

        switch ( edge->edge_type )
        {
        case SDF_EDGE_LINE:
        {
          /* Just create a duplicate edge in case   */
          /* it is a line. We can use the same edge */
          FT_CALL( sdf_edge_new( memory, &temp ) );

          ft_memcpy( temp, edge, sizeof( *edge ) );

          temp->next = new_edges;
          new_edges = temp;
          break;
        }
        case SDF_EDGE_CONIC:
        {
          /* Subdivide the curve and add to the list. */
          FT_26D6_Vec  ctrls[3];


          ctrls[0] = edge->start_pos;
          ctrls[1] = edge->control_a;
          ctrls[2] = edge->end_pos;
          error = split_sdf_conic( memory, ctrls, 16, &new_edges );
          break;
        }
        case SDF_EDGE_CUBIC:
        {
          /* Subdivide the curve and add to the list. */
          FT_26D6_Vec  ctrls[4];


          ctrls[0] = edge->start_pos;
          ctrls[1] = edge->control_a;
          ctrls[2] = edge->control_b;
          ctrls[3] = edge->end_pos;
          error = split_sdf_cubic( memory, ctrls, 16, &new_edges );
          break;
        }
        default:
          error = FT_THROW( Invalid_Argument );
          goto Exit;
        }

        edges = edges->next;
      }

      /* add to the contours list */
      FT_CALL( sdf_contour_new( memory, &tempc ) );
      tempc->next   = new_contours;
      tempc->edges  = new_edges;
      new_contours  = tempc;
      new_edges     = NULL;

      /* deallocate the contour */
      tempc = contours;
      contours = contours->next;

      sdf_contour_done( memory, &tempc );
    }

    shape->contours = new_contours;

  Exit:
    return error;
  }

  /**************************************************************************
   *
   * for debugging
   *
   */

#ifdef FT_DEBUG_LEVEL_TRACE

  static void
  sdf_shape_dump( SDF_Shape*  shape )
  {
    FT_UInt     num_contours = 0;
    FT_UInt     total_edges  = 0;
    FT_UInt     total_lines  = 0;
    FT_UInt     total_conic  = 0;
    FT_UInt     total_cubic  = 0;

    SDF_Contour*  contour_list;

    if ( !shape )
    {
      FT_TRACE5(( "[sdf] sdf_shape_dump: null shape\n" ));
      return;
    }

    contour_list = shape->contours;

    FT_TRACE5(( "-------------------------------------------------\n" ));
    FT_TRACE5(( "[sdf] sdf_shape_dump:\n" ));

    while ( contour_list )
    {
      FT_UInt       num_edges = 0;
      SDF_Edge*     edge_list;
      SDF_Contour*  contour = contour_list;


      edge_list = contour->edges;
      FT_TRACE5(( "Contour %d\n", num_contours ));

      while ( edge_list )
      {
        SDF_Edge*  edge = edge_list;


        FT_TRACE5(( "  Edge %d\n", num_edges ));

        switch (edge->edge_type) {
        case SDF_EDGE_LINE:
          FT_TRACE5(( "    Edge Type: Line\n" ));
          FT_TRACE5(( "    ---------------\n" ));
          FT_TRACE5(( "    Start Pos: %ld, %ld\n", edge->start_pos.x,
                                                   edge->start_pos.y ));
          FT_TRACE5(( "    End Pos  : %ld, %ld\n", edge->end_pos.x,
                                                   edge->end_pos.y ));
          total_lines++;
          break;
        case SDF_EDGE_CONIC:
          FT_TRACE5(( "    Edge Type: Conic Bezier\n" ));
          FT_TRACE5(( "    -----------------------\n" ));
          FT_TRACE5(( "    Start Pos: %ld, %ld\n", edge->start_pos.x,
                                                   edge->start_pos.y ));
          FT_TRACE5(( "    Ctrl1 Pos: %ld, %ld\n", edge->control_a.x,
                                                   edge->control_a.y ));
          FT_TRACE5(( "    End Pos  : %ld, %ld\n", edge->end_pos.x,
                                                   edge->end_pos.y ));
          total_conic++;
          break;
        case SDF_EDGE_CUBIC:
          FT_TRACE5(( "    Edge Type: Cubic Bezier\n" ));
          FT_TRACE5(( "    -----------------------\n" ));
          FT_TRACE5(( "    Start Pos: %ld, %ld\n", edge->start_pos.x,
                                                   edge->start_pos.y ));
          FT_TRACE5(( "    Ctrl1 Pos: %ld, %ld\n", edge->control_a.x,
                                                   edge->control_a.y ));
          FT_TRACE5(( "    Ctrl2 Pos: %ld, %ld\n", edge->control_b.x,
                                                   edge->control_b.y ));
          FT_TRACE5(( "    End Pos  : %ld, %ld\n", edge->end_pos.x,
                                                   edge->end_pos.y ));
          total_cubic++;
          break;
        default:
            break;
        }

        num_edges++;
        total_edges++;
        edge_list = edge_list->next;
      }

      num_contours++;
      contour_list = contour_list->next;
    }

    FT_TRACE5(( "\n" ));
    FT_TRACE5(( "*note: the above values are "
                "in 26.6 fixed point format*\n" ));
    FT_TRACE5(( "total number of contours = %d\n", num_contours ));
    FT_TRACE5(( "total number of edges    = %d\n", total_edges ));
    FT_TRACE5(( "  |__lines = %d\n", total_lines ));
    FT_TRACE5(( "  |__conic = %d\n", total_conic ));
    FT_TRACE5(( "  |__cubic = %d\n", total_cubic ));
    FT_TRACE5(( "[sdf] sdf_shape_dump complete\n" ));
    FT_TRACE5(( "-------------------------------------------------\n" ));
  }

#endif

  /**************************************************************************
   *
   * math functions
   *
   */

  /* Original Algorithm: https://github.com/chmike/fpsqrt */
  static FT_16D16
  square_root( FT_16D16  val )
  {
    FT_ULong t, q, b, r;


    r = val;
    b = 0x40000000;
    q = 0;
    while( b > 0x40 )
    {
      t = q + b;
      if( r >= t )
      {
        r -= t;
        q = t + b;
      }
      r <<= 1;
      b >>= 1;
    }
    q >>= 8;

    return q;
  }

#if !USE_NEWTON_FOR_CONIC

  /* [NOTE]: All the functions below down until rasterizer */
  /*         can be avoided if we decide to subdivide the  */
  /*         curve into lines.                             */

  /* This function uses newton's iteration to find */
  /* cube root of a fixed point integer.           */
  static FT_16D16
  cube_root( FT_16D16  val )
  {
    /* [IMPORTANT]: This function is not good as it may */
    /* not break, so use a lookup table instead. Or we  */
    /* can use algorithm similar to `square_root'.      */

    FT_Int  v, g, c;


    if ( val == 0 ||
         val == -FT_INT_16D16( 1 ) ||
         val ==  FT_INT_16D16( 1 ) )
      return val;

    v = val < 0 ? -val : val;
    g = square_root( v );
    c = 0;

    while ( 1 )
    {
      c = FT_MulFix( FT_MulFix( g, g ), g ) - v;
      c = FT_DivFix( c, 3 * FT_MulFix( g, g ) );

      g -= c;

      if ( ( c < 0 ? -c : c ) < 30 )
        break;
    }

    return val < 0 ? -g : g;
  }

  /* The function calculate the perpendicular */
  /* using 1 - ( base ^ 2 ) and then use arc  */
  /* tan to compute the angle.                */
  static FT_16D16
  arc_cos( FT_16D16  val )
  {
    FT_16D16  p, b = val;
    FT_16D16  one  = FT_INT_16D16( 1 );


    if ( b >  one ) b =  one;
    if ( b < -one ) b = -one;

    p = one - FT_MulFix( b, b );
    p = square_root( p );

    return FT_Atan2( b, p );
  }

  /* The function compute the roots of a quadratic       */
  /* polynomial, assigns it to `out' and returns the     */
  /* number of real roots of the equation.               */
  /* The procedure can be found at:                      */
  /* https://mathworld.wolfram.com/QuadraticFormula.html */
  static FT_UShort
  solve_quadratic_equation( FT_26D6   a,
                            FT_26D6   b,
                            FT_26D6   c,
                            FT_16D16  out[2] )
  {
    FT_16D16  discriminant = 0;


    a = FT_26D6_16D16( a );
    b = FT_26D6_16D16( b );
    c = FT_26D6_16D16( c );

    if ( a == 0 )
    {
      if ( b == 0 )
        return 0;
      else 
      {
        out[0] = FT_DivFix( -c, b );
        return 1;
      }
    }

    discriminant = FT_MulFix( b, b ) - 4 * FT_MulFix( a, c );

    if ( discriminant < 0 )
      return 0;
    else if ( discriminant == 0 )
    {
      out[0] = FT_DivFix( -b, 2 * a );

      return 1;
    }
    else
    {
      discriminant = square_root( discriminant );
      out[0] = FT_DivFix( -b + discriminant, 2 * a );
      out[1] = FT_DivFix( -b - discriminant, 2 * a );

      return 2;
    }
  }

  /* The function compute the roots of a cubic polynomial */
  /* assigns it to `out' and returns the number of real   */
  /* roots of the equation.                               */
  /* The procedure can be found at:                       */
  /* https://mathworld.wolfram.com/CubicFormula.html      */
  static FT_UShort
  solve_cubic_equation( FT_26D6   a,
                        FT_26D6   b,
                        FT_26D6   c,
                        FT_26D6   d,
                        FT_16D16  out[3] )
  {
    FT_16D16  q              = 0;     /* intermediate      */
    FT_16D16  r              = 0;     /* intermediate      */

    FT_16D16  a2             = b;     /* x^2 coefficients  */
    FT_16D16  a1             = c;     /* x coefficients    */
    FT_16D16  a0             = d;     /* constant          */

    FT_16D16  q3             = 0;
    FT_16D16  r2             = 0;
    FT_16D16  a23            = 0;
    FT_16D16  a22            = 0;
    FT_16D16  a1x2           = 0;


    /* cutoff value for `a' to be a cubic otherwise solve quadratic*/
    if ( a == 0 || FT_ABS( a ) < 16 )
      return solve_quadratic_equation( b, c, d, out );
    if ( d == 0 )
    {
      out[0] = 0;
      return solve_quadratic_equation( a, b, c, out + 1 ) + 1;
    }

    /* normalize the coefficients, this also makes them 16.16 */
    a2 = FT_DivFix( a2, a );
    a1 = FT_DivFix( a1, a );
    a0 = FT_DivFix( a0, a );

    /* compute intermediates */
    a1x2 = FT_MulFix( a1, a2 );
    a22 = FT_MulFix( a2, a2 );
    a23 = FT_MulFix( a22, a2 );

    q = ( 3 * a1 - a22 ) / 9;
    r = ( 9 * a1x2 - 27 * a0 - 2 * a23 ) / 54;

    /* [BUG]: `q3' and `r2' still causes underflow. */

    q3 = FT_MulFix( q, q );
    q3 = FT_MulFix( q3, q );

    r2 = FT_MulFix( r, r );

    if ( q3 < 0 && r2 < -q3 )
    {
      FT_16D16  t = 0;


      q3 = square_root( -q3 );
      t = FT_DivFix( r, q3 );
      if ( t >  ( 1 << 16 ) ) t =  ( 1 << 16 );
      if ( t < -( 1 << 16 ) ) t = -( 1 << 16 );

      t = arc_cos( t );
      a2 /= 3;
      q = 2 * square_root( -q );
      out[0] = FT_MulFix( q, FT_Cos( t / 3 ) ) - a2;
      out[1] = FT_MulFix( q, FT_Cos( ( t + FT_ANGLE_PI * 2 ) / 3 ) ) - a2;
      out[2] = FT_MulFix( q, FT_Cos( ( t + FT_ANGLE_PI * 4 ) / 3 ) ) - a2;

      return 3;
    }
    else if ( r2 == -q3 )
    {
      FT_16D16  s = 0;


      s = cube_root( r );
      a2 /= -3;
      out[0] = a2 + ( 2 * s );
      out[1] = a2 - s;

      return 2;
    }
    else
    {
      FT_16D16  s    = 0;
      FT_16D16  t    = 0;
      FT_16D16  dis  = 0;


      if ( q3 == 0 )
        dis = FT_ABS( r );
      else
        dis = square_root( q3 + r2 );

      s = cube_root( r + dis );
      t = cube_root( r - dis );
      a2 /= -3;
      out[0] = ( a2 + ( s + t ) );

      return 1;
    }
  }

#endif

  /*************************************************************************/
  /*************************************************************************/
  /**                                                                     **/
  /**  RASTERIZER                                                         **/
  /**                                                                     **/
  /*************************************************************************/
  /*************************************************************************/

  /**************************************************************************
   *
   * @Function:
   *   resolve_corner
   *
   * @Description:
   *   At some places on the grid two edges can give opposite direction
   *   this happens when the closes point is on of the endpoint, in that
   *   case we need to check the proper sign.
   *
   *   This can be visualized by an example:
   *
   *                x
   *                 
   *                   o
   *                  ^ \
   *                 /   \
   *                /     \
   *           (a) /       \  (b)
   *              /         \
   *             /           \
   *            /             v
   *
   *   Suppose `x' is the point whose shortest distance from an arbitrary
   *   contour we want to find out. It is clear that `o' is the nearest
   *   point on the contour. Now to determine the sign we do a cross
   *   product of shortest distance vector and the edge direction. i.e.
   *
   *   => sign = cross( ( x - o ), direction( a ) )
   *
   *   From right hand thumb rule we can see that the sign will be positive
   *   and if check for `b'.
   *
   *   => sign = cross( ( x - o ), direction( b ) )
   *
   *   In this case the sign will be negative. So, to determine the correct
   *   sign we divide the plane in half and check in which plane the point
   *   lies.
   *
   *   Divide:
   *
   *                   |
   *                x  |
   *                   |
   *                   o
   *                  ^|\
   *                 / | \
   *                /  |  \
   *           (a) /   |   \  (b)
   *              /    |    \
   *             /           \
   *            /             v
   *
   *   We can see that `x' lies in the plane of `a', so we take the sign
   *   determined by `a'. This can be easily done by calculating the 
   *   orthogonality and taking the greater one.
   *   The orthogonality is nothing but the sinus of the two vectors (i.e.
   *   ( x - o ) and the corresponding direction. The orthogonality is pre
   *   computed by the corresponding `get_min_distance_' functions efficiently.
   *
   * @Input:
   *   [TODO]
   *
   * @Return:
   *   [TODO]
   */
  static SDF_Signed_Distance
  resolve_corner( SDF_Signed_Distance  sdf1,
                  SDF_Signed_Distance  sdf2 )
  {
    return FT_ABS( sdf1.cross ) > FT_ABS( sdf2.cross ) ? sdf1 : sdf2;
  }

  /**************************************************************************
   *
   * @Function:
   *   get_min_distance_line
   *
   * @Description:
   *   This function find the shortest distance from the `line' to
   *   a given `point' and assigns it to `out'. Only use it for line
   *   segments.
   *
   * @Input:
   *   [TODO]
   *
   * @Return:
   *   [TODO]
   */
  static FT_Error
  get_min_distance_line( SDF_Edge*             line,
                         FT_26D6_Vec           point, 
                         SDF_Signed_Distance*  out )
  {
    /* in order to calculate the shortest distance from a point to */
    /* a line segment.                                             */
    /*                                                             */
    /* a = start point of the line segment                         */
    /* b = end point of the line segment                           */
    /* p = point from which shortest distance is to be calculated  */
    /* ----------------------------------------------------------- */
    /* => we first write the parametric equation of the line       */
    /*    point_on_line = a + ( b - a ) * t ( t is the factor )    */
    /*                                                             */
    /* => next we find the projection of point p on the line. the  */
    /*    projection will be perpendicular to the line, that is    */
    /*    why we can find it by making the dot product zero.       */
    /*    ( point_on_line - a ) . ( p - point_on_line ) = 0        */
    /*                                                             */
    /*                 ( point_on_line )                           */
    /*    ( a ) x-------o----------------x ( b )                   */
    /*                |_|                                          */
    /*                  |                                          */
    /*                  |                                          */
    /*                ( p )                                        */
    /*                                                             */
    /* => by simplifying the above equation we get the factor of   */
    /*    point_on_line such that                                  */
    /*    t = ( ( p - a ) . ( b - a ) ) / ( |b - a| ^ 2 )          */
    /*                                                             */
    /* => we clamp the factor t between [0.0f, 1.0f], because the  */
    /*    point_on_line can be outside the line segment.           */
    /*                                                             */
    /*                                        ( point_on_line )    */
    /*    ( a ) x------------------------x ( b ) -----o---         */
    /*                                              |_|            */
    /*                                                |            */
    /*                                                |            */
    /*                                              ( p )          */
    /*                                                             */
    /* => finally the distance becomes | point_on_line - p |       */

    FT_Error     error = FT_Err_Ok;

    FT_Vector    a;                 /* start position */
    FT_Vector    b;                 /* end position   */
    FT_Vector    p;                 /* current point  */

    FT_26D6_Vec  line_segment;      /* `b' - `a'*/
    FT_26D6_Vec  p_sub_a;           /* `p' - `a' */

    FT_26D6      sq_line_length;    /* squared length of `line_segment' */
    FT_16D16     factor;            /* factor of the nearest point      */
    FT_26D6      cross;             /* used to determine sign           */

    FT_16D16_Vec nearest_point;     /* `point_on_line'       */
    FT_16D16_Vec nearest_vector;    /* `p' - `nearest_point' */


    if ( !line || !out )
    {
      error = FT_THROW( Invalid_Argument );
      goto Exit;
    }

    if ( line->edge_type != SDF_EDGE_LINE )
    {
      error = FT_THROW( Invalid_Argument );
      goto Exit;
    }

    a = line->start_pos;
    b = line->end_pos;
    p = point;

    line_segment.x = b.x - a.x;
    line_segment.y = b.y - a.y;

    p_sub_a.x = p.x - a.x;
    p_sub_a.y = p.y - a.y;

    sq_line_length = ( line_segment.x * line_segment.x ) / 64 +
                     ( line_segment.y * line_segment.y ) / 64;

    /* currently factor is 26.6 */
    factor = ( p_sub_a.x * line_segment.x ) / 64 +
             ( p_sub_a.y * line_segment.y ) / 64;

    /* now factor is 16.16 */
    factor = FT_DivFix( factor, sq_line_length );

    /* clamp the factor between 0.0 and 1.0 in fixed point */
    if ( factor > FT_INT_16D16( 1 ) )
      factor = FT_INT_16D16( 1 );
    if ( factor < 0 )
      factor = 0;

    nearest_point.x = FT_MulFix( FT_26D6_16D16(line_segment.x),
                                 factor );
    nearest_point.y = FT_MulFix( FT_26D6_16D16(line_segment.y),
                                 factor );

    nearest_point.x = FT_26D6_16D16( a.x ) + nearest_point.x; 
    nearest_point.y = FT_26D6_16D16( a.y ) + nearest_point.y;

    nearest_vector.x = nearest_point.x - FT_26D6_16D16( p.x );
    nearest_vector.y = nearest_point.y - FT_26D6_16D16( p.y );

    cross = FT_MulFix( nearest_vector.x, line_segment.y ) -
            FT_MulFix( nearest_vector.y, line_segment.x );

    /* assign the output */
    out->sign = cross < 0 ? 1 : -1;
    out->distance = VECTOR_LENGTH_16D16( nearest_vector );

    /* Instead of finding cross for checking corner we */
    /* directly set it here. This is more efficient    */
    /* because if the distance is perpendicular we can */
    /* directly set it to 1.                           */
    if ( factor != 0 && factor != FT_INT_16D16( 1 ) )
      out->cross = FT_INT_16D16( 1 );
    else
    {
      /* [OPTIMIZATION]: Pre-compute this direction. */
      /* if not perpendicular then compute the cross */
      FT_Vector_NormLen( &line_segment );
      FT_Vector_NormLen( &nearest_vector );

      out->cross = FT_MulFix( line_segment.x, nearest_vector.y ) -
                   FT_MulFix( line_segment.y, nearest_vector.x );
    }

  Exit:
    return error;
  }

#if !USE_NEWTON_FOR_CONIC

  /**************************************************************************
   *
   * @Function:
   *   get_min_distance_conic
   *
   * @Description:
   *   This function find the shortest distance from the `conic' bezier
   *   curve to a given `point' and assigns it to `out'. Only use it for
   *   conic/quadratic curves.
   *   [Note]: The function uses analytical method to find shortest distance
   *           which is faster than the Newton-Raphson's method, but has
   *           underflows at the moment. Use Newton's method if you can
   *           see artifacts in the SDF.
   *
   * @Input:
   *   [TODO]
   *
   * @Return:
   *   [TODO]
   */
  static FT_Error
  get_min_distance_conic( SDF_Edge*             conic,
                          FT_26D6_Vec           point,
                          SDF_Signed_Distance*  out )
  {
      /* The procedure to find the shortest distance from a point to */
      /* a quadratic bezier curve is similar to a line segment. the  */
      /* shortest distance will be perpendicular to the bezier curve */
      /* The only difference from line is that there can be more     */
      /* than one perpendicular and we also have to check the endpo- */
      /* -ints, because the perpendicular may not be the shortest.   */
      /*                                                             */
      /* p0 = first endpoint                                         */
      /* p1 = control point                                          */
      /* p2 = second endpoint                                        */
      /* p = point from which shortest distance is to be calculated  */
      /* ----------------------------------------------------------- */
      /* => the equation of a quadratic bezier curve can be written  */
      /*    B( t ) = ( ( 1 - t )^2 )p0 + 2( 1 - t )tp1 + t^2p2       */
      /*    here t is the factor with range [0.0f, 1.0f]             */
      /*    the above equation can be rewritten as                   */
      /*    B( t ) = t^2( p0 - 2p1 + p2 ) + 2t( p1 - p0 ) + p0       */
      /*                                                             */
      /*    now let A = ( p0 - 2p1 + p2), B = ( p1 - p0 )            */
      /*    B( t ) = t^2( A ) + 2t( B ) + p0                         */
      /*                                                             */
      /* => the derivative of the above equation is written as       */
      /*    B'( t ) = 2( tA + B )                                    */
      /*                                                             */
      /* => now to find the shortest distance from p to B( t ), we   */
      /*    find the point on the curve at which the shortest        */
      /*    distance vector ( i.e. B( t ) - p ) and the direction    */
      /*    ( i.e. B'( t )) makes 90 degrees. in other words we make */
      /*    the dot product zero.                                    */
      /*    ( B( t ) - p ).( B'( t ) ) = 0                           */
      /*    ( t^2( A ) + 2t( B ) + p0 - p ).( 2( tA + B ) ) = 0      */
      /*                                                             */
      /*    after simplifying we get a cubic equation as             */
      /*    at^3 + bt^2 + ct + d = 0                                 */
      /*    a = ( A.A ), b = ( 3A.B ), c = ( 2B.B + A.p0 - A.p )     */
      /*    d = ( p0.B - p.B )                                       */
      /*                                                             */
      /* => now the roots of the equation can be computed using the  */
      /*    `Cardano's Cubic formula' we clamp the roots in range    */
      /*    [0.0f, 1.0f].                                            */
      /*                                                             */
      /* [note]: B and B( t ) are different in the above equations   */

    FT_Error     error = FT_Err_Ok;

    FT_26D6_Vec  aA, bB;         /* A, B in the above comment             */
    FT_26D6_Vec  nearest_point;  /* point on curve nearest to `point'     */
    FT_26D6_Vec  direction;      /* direction of curve at `nearest_point' */

    FT_26D6_Vec  p0, p1, p2;     /* control points of a conic curve       */
    FT_26D6_Vec  p;              /* `point' to which shortest distance    */

    FT_26D6      a, b, c, d;     /* cubic coefficients                    */

    FT_16D16     roots[3] = { 0, 0, 0 }; /* real roots of the cubic eq    */
    FT_16D16     min_factor;             /* factor at `nearest_point'     */
    FT_16D16     cross;                  /* to determine the sign         */
    FT_16D16     min = FT_INT_MAX;       /* shortest squared distance     */

    FT_UShort    num_roots;              /* number of real roots of cubic */
    FT_UShort    i;


    if ( !conic || !out )
    {
      error = FT_THROW( Invalid_Argument );
      goto Exit;
    }

    if ( conic->edge_type != SDF_EDGE_CONIC )
    {
      error = FT_THROW( Invalid_Argument );
      goto Exit;
    }

    /* assign the values after checking pointer */
    p0 = conic->start_pos;
    p1 = conic->control_a;
    p2 = conic->end_pos;
    p  = point;

    /* compute substitution coefficients */
    aA.x = p0.x - 2 * p1.x + p2.x;
    aA.y = p0.y - 2 * p1.y + p2.y;

    bB.x = p1.x - p0.x;
    bB.y = p1.y - p0.y;

    /* compute cubic coefficients */
    a = VEC_26D6_DOT( aA, aA );

    b = 3 * VEC_26D6_DOT( aA, bB );

    c = 2 * VEC_26D6_DOT( bB, bB ) +
            VEC_26D6_DOT( aA, p0 ) -
            VEC_26D6_DOT( aA, p );

    d = VEC_26D6_DOT( p0, bB ) -
        VEC_26D6_DOT( p, bB );

    /* find the roots */
    num_roots = solve_cubic_equation( a, b, c, d, roots );

    if ( num_roots == 0 )
    {
      roots[0] = 0;
      roots[1] = FT_INT_16D16( 1 );
      num_roots = 2;
    }

    /* [OPTIMIZATION]: Check the roots, clamp them and discard */
    /*                 duplicate roots.                        */

    /* convert these values to 16.16 for further computation */
    aA.x = FT_26D6_16D16( aA.x );
    aA.y = FT_26D6_16D16( aA.y );

    bB.x = FT_26D6_16D16( bB.x );
    bB.y = FT_26D6_16D16( bB.y );

    p0.x = FT_26D6_16D16( p0.x );
    p0.y = FT_26D6_16D16( p0.y );

    p.x = FT_26D6_16D16( p.x );
    p.y = FT_26D6_16D16( p.y );

    for ( i = 0; i < num_roots; i++ )
    {
      FT_16D16      t    = roots[i];
      FT_16D16      t2   = 0;
      FT_16D16      dist = 0;

      FT_16D16_Vec  curve_point;
      FT_16D16_Vec  dist_vector;

      /* Ideally we should discard the roots which are outside the     */
      /* range [0.0, 1.0] and check the endpoints of the bezier, but   */
      /* Behdad gave me a lemma:                                       */
      /* Lemma:                                                        */
      /* * If the closest point on the curve [0, 1] is to the endpoint */
      /*   at t = 1 and the cubic has no real roots at t = 1 then, the */
      /*   cubic must have a real root at some t > 1.                  */
      /* * Similarly,                                                  */
      /*   If the closest point on the curve [0, 1] is to the endpoint */
      /*   at t = 0 and the cubic has no real roots at t = 0 then, the */
      /*   cubic must have a real root at some t < 0.                  */
      /*                                                               */
      /* Now because of this lemma we only need to clamp the roots and */
      /* that will take care of the endpoints.                         */
      /*                                                               */
      /* For proof contact: behdad@behdad.org                          */
      /* For more details check message:                               */
      /* https://lists.nongnu.org/archive/html/freetype-devel/2020-06/msg00147.html */
      if ( t < 0 )
        t = 0;
      if ( t > FT_INT_16D16( 1 ) )
        t = FT_INT_16D16( 1 );

      t2 = FT_MulFix( t, t );

      /* B( t ) = t^2( A ) + 2t( B ) + p0 - p */
      curve_point.x = FT_MulFix( aA.x, t2 ) +
                      2 * FT_MulFix( bB.x, t ) + p0.x;
      curve_point.y = FT_MulFix( aA.y, t2 ) +
                      2 * FT_MulFix( bB.y, t ) + p0.y;

      /* `curve_point' - `p' */
      dist_vector.x = curve_point.x - p.x;
      dist_vector.y = curve_point.y - p.y;

      dist = VECTOR_LENGTH_16D16( dist_vector );

      if ( dist < min )
      {
        min = dist;
        nearest_point = curve_point;
        min_factor = t;
      }
    }

    /* B'( t ) = 2( tA + B ) */
    direction.x = 2 * FT_MulFix( aA.x, min_factor ) + 2 * bB.x;
    direction.y = 2 * FT_MulFix( aA.y, min_factor ) + 2 * bB.y;

    /* determine the sign */
    cross = FT_MulFix( nearest_point.x - p.x, direction.y ) -
            FT_MulFix( nearest_point.y - p.y, direction.x );

    /* assign the values */
    out->distance = min;
    out->sign = cross < 0 ? 1 : -1;

    if ( min_factor != 0 && min_factor != FT_INT_16D16( 1 ) )
      out->cross = FT_INT_16D16( 1 );   /* the two are perpendicular */
    else
    {
      /* convert to nearest vector */
      nearest_point.x -= FT_26D6_16D16( p.x );
      nearest_point.y -= FT_26D6_16D16( p.y );

      /* if not perpendicular then compute the cross */
      FT_Vector_NormLen( &direction );
      FT_Vector_NormLen( &nearest_point );

      out->cross = FT_MulFix( direction.x, nearest_point.y ) -
                   FT_MulFix( direction.y, nearest_point.x );
    }
  Exit:
    return error;
  }

#else

  /**************************************************************************
   *
   * @Function:
   *   get_min_distance_conic
   *
   * @Description:
   *   This function find the shortest distance from the `conic' bezier
   *   curve to a given `point' and assigns it to `out'. Only use it for
   *   conic/quadratic curves.
   *   [Note]: The function uses Newton's approximation to find the shortest
   *           distance, which is a bit slower than the analytical method
   *           doesn't cause underflow. Use is upto your needs.
   *
   * @Input:
   *   [TODO]
   *
   * @Return:
   *   [TODO]
   */
  static FT_Error
  get_min_distance_conic( SDF_Edge*             conic,
                          FT_26D6_Vec           point,
                          SDF_Signed_Distance*  out )
  {
    /* This method uses Newton-Raphson's approximation to find the */
    /* shortest distance from a point to conic curve which does    */
    /* not involve solving any cubic equation, that is why there   */
    /* is no risk of underflow. The method is as follows:          */
    /*                                                             */
    /* p0 = first endpoint                                         */
    /* p1 = control point                                          */
    /* p3 = second endpoint                                        */
    /* p  = point from which shortest distance is to be calculated */
    /* ----------------------------------------------------------- */
    /* => the equation of a quadratic bezier curve can be written  */
    /*    B( t ) = ( ( 1 - t )^2 )p0 + 2( 1 - t )tp1 + t^2p2       */
    /*    here t is the factor with range [0.0f, 1.0f]             */
    /*    the above equation can be rewritten as                   */
    /*    B( t ) = t^2( p0 - 2p1 + p2 ) + 2t( p1 - p0 ) + p0       */
    /*                                                             */
    /*    now let A = ( p0 - 2p1 + p2), B = 2( p1 - p0 )           */
    /*    B( t ) = t^2( A ) + t( B ) + p0                          */
    /*                                                             */
    /* => the derivative of the above equation is written as       */
    /*    B'( t ) = 2t( A ) + B                                    */
    /*                                                             */
    /* => further derivative of the above equation is written as   */
    /*    B''( t ) = 2A                                            */
    /*                                                             */
    /* => the equation of distance from point `p' to the curve     */
    /*    P( t ) can be written as                                 */
    /*    P( t ) = t^2( A ) + t^2( B ) + p0 - p                    */
    /*    Now let C = ( p0 - p )                                   */
    /*    P( t ) = t^2( A ) + t( B ) + C                           */
    /*                                                             */
    /* => finally the equation of angle between curve B( t ) and   */
    /*    point to curve distance P( t ) can be written as         */
    /*    Q( t ) = P( t ).B'( t )                                  */
    /*                                                             */
    /* => now our task is to find a value of t such that the above */
    /*    equation Q( t ) becomes zero. in other words the point   */
    /*    to curve vector makes 90 degree with curve. this is done */
    /*    by Newton-Raphson's method.                              */
    /*                                                             */
    /* => we first assume a arbitary value of the factor `t' and   */
    /*    then we improve it using Newton's equation such as       */
    /*                                                             */
    /*    t -= Q( t ) / Q'( t )                                    */
    /*    putting value of Q( t ) from the above equation gives    */
    /*                                                             */
    /*    t -= P( t ).B'( t ) / derivative( P( t ).B'( t ) )       */
    /*    t -= P( t ).B'( t ) /                                    */
    /*         ( P'( t )B'( t ) + P( t ).B''( t ) )                */
    /*                                                             */
    /*    P'( t ) is noting but B'( t ) because the constant are   */
    /*    gone due to derivative                                   */
    /*                                                             */
    /* => finally we get the equation to improve the factor as     */
    /*    t -= P( t ).B'( t ) /                                    */
    /*         ( B'( t ).B'( t ) + P( t ).B''( t ) )               */
    /*                                                             */
    /* [note]: B and B( t ) are different in the above equations   */

    FT_Error     error = FT_Err_Ok;

    FT_26D6_Vec  aA, bB, cC;     /* A, B, C in the above comment          */
    FT_26D6_Vec  nearest_point;  /* point on curve nearest to `point'     */
    FT_26D6_Vec  direction;      /* direction of curve at `nearest_point' */

    FT_26D6_Vec  p0, p1, p2;     /* control points of a conic curve       */
    FT_26D6_Vec  p;              /* `point' to which shortest distance    */

    FT_16D16     min_factor = 0;         /* factor at `nearest_point'     */
    FT_16D16     cross;                  /* to determine the sign         */
    FT_16D16     min = FT_INT_MAX;       /* shortest squared distance     */

    FT_UShort    iterations;
    FT_UShort    steps;


    if ( !conic || !out )
    {
      error = FT_THROW( Invalid_Argument );
      goto Exit;
    }

    if ( conic->edge_type != SDF_EDGE_CONIC )
    {
      error = FT_THROW( Invalid_Argument );
      goto Exit;
    }

    /* assign the values after checking pointer */
    p0 = conic->start_pos;
    p1 = conic->control_a;
    p2 = conic->end_pos;
    p  = point;

    /* compute substitution coefficients */
    aA.x = p0.x - 2 * p1.x + p2.x;
    aA.y = p0.y - 2 * p1.y + p2.y;

    bB.x = 2 * ( p1.x - p0.x );
    bB.y = 2 * ( p1.y - p0.y );

    cC.x = p0.x;
    cC.y = p0.y;

    /* do newton's iterations */
    for ( iterations = 0; iterations <= MAX_NEWTON_DIVISIONS; iterations++ )
    {
      FT_16D16  factor = FT_INT_16D16( iterations ) / MAX_NEWTON_DIVISIONS;
      FT_16D16  factor2;
      FT_16D16  length;

      FT_16D16_Vec  curve_point; /* point on the curve  */
      FT_16D16_Vec  dist_vector; /* `curve_point' - `p' */

      FT_26D6_Vec   d1;          /* first  derivative   */
      FT_26D6_Vec   d2;          /* second derivative   */

      FT_16D16      temp1;
      FT_16D16      temp2;

      for ( steps = 0; steps < MAX_NEWTON_STEPS; steps++ )
      {
        factor2 = FT_MulFix( factor, factor );

        /* B( t ) = t^2( A ) + t( B ) + p0 */
        curve_point.x = FT_MulFix( aA.x, factor2 ) +
                        FT_MulFix( bB.x, factor ) + cC.x;
        curve_point.y = FT_MulFix( aA.y, factor2 ) +
                        FT_MulFix( bB.y, factor ) + cC.y;

        /* convert to 16.16 */
        curve_point.x = FT_26D6_16D16( curve_point.x );
        curve_point.y = FT_26D6_16D16( curve_point.y );

        /* B( t ) = t^2( A ) + t( B ) + p0 - p. P( t ) in the comment */
        dist_vector.x = curve_point.x - FT_26D6_16D16( p.x );
        dist_vector.y = curve_point.y - FT_26D6_16D16( p.y );

        length = VECTOR_LENGTH_16D16( dist_vector );

        if ( length < min )
        {
          min = length;
          min_factor = factor;
          nearest_point = curve_point;
        }

        /* This the actual Newton's approximation.       */
        /*    t -= P( t ).B'( t ) /                      */
        /*         ( B'( t ).B'( t ) + P( t ).B''( t ) ) */

        /* B'( t ) = 2tA + B */
        d1.x = FT_MulFix( aA.x, 2 * factor ) + bB.x;
        d1.y = FT_MulFix( aA.y, 2 * factor ) + bB.y;

        /* B''( t ) = 2A */
        d2.x = 2 * aA.x;
        d2.y = 2 * aA.y;

        dist_vector.x /= 1024;
        dist_vector.y /= 1024;

        /* temp1 = P( t ).B'( t ) */
        temp1 = VEC_26D6_DOT( dist_vector, d1 );

        /* temp2 = ( B'( t ).B'( t ) + P( t ).B''( t ) ) */
        temp2 = VEC_26D6_DOT( d1, d1 ) +
                VEC_26D6_DOT( dist_vector, d2 );

        factor -= FT_DivFix( temp1, temp2 );

        if ( factor < 0 || factor > FT_INT_16D16( 1 ) )
          break;
      }
    }

    /* B'( t ) = 2tA + B */
    direction.x = 2 * FT_MulFix( aA.x, min_factor ) + bB.x;
    direction.y = 2 * FT_MulFix( aA.y, min_factor ) + bB.y;

    /* determine the sign */
    cross = FT_MulFix( nearest_point.x - FT_26D6_16D16( p.x ), direction.y ) -
            FT_MulFix( nearest_point.y - FT_26D6_16D16( p.y ), direction.x );

    /* assign the values */
    out->distance = min;
    out->sign = cross < 0 ? 1 : -1;

    if ( min_factor != 0 && min_factor != FT_INT_16D16( 1 ) )
      out->cross = FT_INT_16D16( 1 );   /* the two are perpendicular */
    else
    {
      /* convert to nearest vector */
      nearest_point.x -= FT_26D6_16D16( p.x );
      nearest_point.y -= FT_26D6_16D16( p.y );

      /* if not perpendicular then compute the cross */
      FT_Vector_NormLen( &direction );
      FT_Vector_NormLen( &nearest_point );

      out->cross = FT_MulFix( direction.x, nearest_point.y ) -
                   FT_MulFix( direction.y, nearest_point.x );
    }

  Exit:
    return error;
  }

#endif

  /**************************************************************************
   *
   * @Function:
   *   get_min_distance_cubic
   *
   * @Description:
   *   This function find the shortest distance from the `cubic' bezier
   *   curve to a given `point' and assigns it to `out'. Only use it for
   *   cubic curves.
   *   [Note]: The function uses Newton's approximation to find the shortest
   *           distance. Another way would be to divide the cubic into conic
   *           or subdivide the curve into lines.
   *
   * @Input:
   *   [TODO]
   *
   * @Return:
   *   [TODO]
   */
  static FT_Error
  get_min_distance_cubic( SDF_Edge*             cubic,
                          FT_26D6_Vec           point,
                          SDF_Signed_Distance*  out )
  {
    /* the procedure to find the shortest distance from a point to */
    /* a cubic bezier curve is similar to a quadratic curve.       */
    /* The only difference is that while calculating the factor    */
    /* `t', instead of a cubic polynomial equation we have to find */
    /* the roots of a 5th degree polynomial equation.              */
    /* But since solving a 5th degree polynomial equation require  */
    /* significant amount of time and still the results may not be */
    /* accurate, we are going to directly approximate the value of */
    /* `t' using Newton-Raphson method                             */
    /*                                                             */
    /* p0 = first endpoint                                         */
    /* p1 = first control point                                    */
    /* p2 = second control point                                   */
    /* p3 = second endpoint                                        */
    /* p = point from which shortest distance is to be calculated  */
    /* ----------------------------------------------------------- */
    /* => the equation of a cubic bezier curve can be written as:  */
    /*    B( t ) = ( ( 1 - t )^3 )p0 + 3( ( 1 - t )^2 )tp1 +       */
    /*             3( 1 - t )( t^2 )p2 + ( t^3 )p3                 */
    /*    The equation can be expanded and written as:             */
    /*    B( t ) = ( t^3 )( -p0 + 3p1 - 3p2 + p3 ) +               */
    /*             3( t^2 )( p0 - 2p1 + p2 ) + 3t( -p0 + p1 ) + p0 */
    /*                                                             */
    /*    Now let A = ( -p0 + 3p1 - 3p2 + p3 ),                    */
    /*            B = 3( p0 - 2p1 + p2 ), C = 3( -p0 + p1 )        */
    /*    B( t ) = t^3( A ) + t^2( B ) + tC + p0                   */
    /*                                                             */
    /* => the derivative of the above equation is written as       */
    /*    B'( t ) = 3t^2( A ) + 2t( B ) + C                        */
    /*                                                             */
    /* => further derivative of the above equation is written as   */
    /*    B''( t ) = 6t( A ) + 2B                                  */
    /*                                                             */
    /* => the equation of distance from point `p' to the curve     */
    /*    P( t ) can be written as                                 */
    /*    P( t ) = t^3( A ) + t^2( B ) + tC + p0 - p               */
    /*    Now let D = ( p0 - p )                                   */
    /*    P( t ) = t^3( A ) + t^2( B ) + tC + D                    */
    /*                                                             */
    /* => finally the equation of angle between curve B( t ) and   */
    /*    point to curve distance P( t ) can be written as         */
    /*    Q( t ) = P( t ).B'( t )                                  */
    /*                                                             */
    /* => now our task is to find a value of t such that the above */
    /*    equation Q( t ) becomes zero. in other words the point   */
    /*    to curve vector makes 90 degree with curve. this is done */
    /*    by Newton-Raphson's method.                              */
    /*                                                             */
    /* => we first assume a arbitary value of the factor `t' and   */
    /*    then we improve it using Newton's equation such as       */
    /*                                                             */
    /*    t -= Q( t ) / Q'( t )                                    */
    /*    putting value of Q( t ) from the above equation gives    */
    /*                                                             */
    /*    t -= P( t ).B'( t ) / derivative( P( t ).B'( t ) )       */
    /*    t -= P( t ).B'( t ) /                                    */
    /*         ( P'( t )B'( t ) + P( t ).B''( t ) )                */
    /*                                                             */
    /*    P'( t ) is noting but B'( t ) because the constant are   */
    /*    gone due to derivative                                   */
    /*                                                             */
    /* => finally we get the equation to improve the factor as     */
    /*    t -= P( t ).B'( t ) /                                    */
    /*         ( B'( t ).B'( t ) + P( t ).B''( t ) )               */
    /*                                                             */
    /* [note]: B and B( t ) are different in the above equations   */

    FT_Error      error = FT_Err_Ok;

    FT_26D6_Vec   aA, bB, cC, dD; /* A, B, C in the above comment          */
    FT_16D16_Vec  nearest_point;  /* point on curve nearest to `point'     */
    FT_16D16_Vec  direction;      /* direction of curve at `nearest_point' */

    FT_26D6_Vec   p0, p1, p2, p3; /* control points of a cubic curve       */
    FT_26D6_Vec   p;              /* `point' to which shortest distance    */

    FT_16D16      min = FT_INT_MAX;  /* shortest distance           */
    FT_16D16      min_factor = 0;    /* factor at shortest distance */
    FT_16D16      min_factor_sq = 0; /* factor at shortest distance */
    FT_16D16      cross;             /* to determine the sign       */

    FT_UShort     iterations;
    FT_UShort     steps;


    if ( !cubic || !out )
    {
      error = FT_THROW( Invalid_Argument );
      goto Exit;
    }

    if ( cubic->edge_type != SDF_EDGE_CUBIC )
    {
      error = FT_THROW( Invalid_Argument );
      goto Exit;
    }

    /* assign the values after checking pointer */
    p0 = cubic->start_pos;
    p1 = cubic->control_a;
    p2 = cubic->control_b;
    p3 = cubic->end_pos;
    p  = point;

    /* compute substitution coefficients */
    aA.x = -p0.x + 3 * ( p1.x - p2.x ) + p3.x;
    aA.y = -p0.y + 3 * ( p1.y - p2.y ) + p3.y;

    bB.x = 3 * ( p0.x - 2 * p1.x + p2.x );
    bB.y = 3 * ( p0.y - 2 * p1.y + p2.y );

    cC.x = 3 * ( p1.x - p0.x );
    cC.y = 3 * ( p1.y - p0.y );

    dD.x = p0.x;
    dD.y = p0.y;

    for ( iterations = 0; iterations <= MAX_NEWTON_DIVISIONS; iterations++ )
    {
      FT_16D16  factor  = FT_INT_16D16( iterations ) / MAX_NEWTON_DIVISIONS;

      FT_16D16  factor2;         /* factor^2            */
      FT_16D16  factor3;         /* factor^3            */
      FT_16D16  length;

      FT_16D16_Vec  curve_point; /* point on the curve  */
      FT_16D16_Vec  dist_vector; /* `curve_point' - `p' */

      FT_26D6_Vec   d1;          /* first  derivative   */
      FT_26D6_Vec   d2;          /* second derivative   */

      FT_16D16      temp1;
      FT_16D16      temp2;


      for ( steps = 0; steps < MAX_NEWTON_STEPS; steps++ )
      {
        factor2 = FT_MulFix( factor, factor );
        factor3 = FT_MulFix( factor2, factor );

        /* B( t ) = t^3( A ) + t^2( B ) + tC + D */
        curve_point.x = FT_MulFix( aA.x, factor3 ) +
                        FT_MulFix( bB.x, factor2 ) +
                        FT_MulFix( cC.x, factor ) + dD.x;
        curve_point.y = FT_MulFix( aA.y, factor3 ) +
                        FT_MulFix( bB.y, factor2 ) +
                        FT_MulFix( cC.y, factor ) + dD.y;

        /* convert to 16.16 */
        curve_point.x = FT_26D6_16D16( curve_point.x );
        curve_point.y = FT_26D6_16D16( curve_point.y );

        /* P( t ) in the comment */
        dist_vector.x = curve_point.x - FT_26D6_16D16( p.x );
        dist_vector.y = curve_point.y - FT_26D6_16D16( p.y );

        length = VECTOR_LENGTH_16D16( dist_vector );

        if ( length < min )
        {
          min = length;
          min_factor = factor;
          min_factor_sq = factor2;
          nearest_point = curve_point;
        }

        /* This the actual Newton's approximation.       */
        /*    t -= P( t ).B'( t ) /                      */
        /*         ( B'( t ).B'( t ) + P( t ).B''( t ) ) */

        /* B'( t ) = 3t^2( A ) + 2t( B ) + C */
        d1.x = FT_MulFix( aA.x, 3 * factor2 ) + 
               FT_MulFix( bB.x, 2 * factor ) + cC.x;
        d1.y = FT_MulFix( aA.y, 3 * factor2 ) + 
               FT_MulFix( bB.y, 2 * factor ) + cC.y;

        /* B''( t ) = 6t( A ) + 2B */
        d2.x = FT_MulFix( aA.x, 6 * factor ) + 2 * bB.x;
        d2.y = FT_MulFix( aA.y, 6 * factor ) + 2 * bB.y;

        dist_vector.x /= 1024;
        dist_vector.y /= 1024;

        /* temp1 = P( t ).B'( t ) */
        temp1 = VEC_26D6_DOT( dist_vector, d1 );

        /* temp2 = ( B'( t ).B'( t ) + P( t ).B''( t ) ) */
        temp2 = VEC_26D6_DOT( d1, d1 ) +
                VEC_26D6_DOT( dist_vector, d2 );

        factor -= FT_DivFix( temp1, temp2 );

        if ( factor < 0 || factor > FT_INT_16D16( 1 ) )
          break;
      }
    }

    /* B'( t ) = 3t^2( A ) + 2t( B ) + C */
    direction.x = FT_MulFix( aA.x, 3 * min_factor_sq ) + 
                  FT_MulFix( bB.x, 2 * min_factor ) + cC.x;
    direction.y = FT_MulFix( aA.y, 3 * min_factor_sq ) + 
                  FT_MulFix( bB.y, 2 * min_factor ) + cC.y;

    /* determine the sign */
    cross = FT_MulFix( nearest_point.x - FT_26D6_16D16( p.x ), direction.y ) -
            FT_MulFix( nearest_point.y - FT_26D6_16D16( p.y ), direction.x );

    /* assign the values */
    out->distance = min;
    out->sign = cross < 0 ? 1 : -1;

    if ( min_factor != 0 && min_factor != FT_INT_16D16( 1 ) )
      out->cross = FT_INT_16D16( 1 );   /* the two are perpendicular */
    else
    {
      /* convert to nearest vector */
      nearest_point.x -= FT_26D6_16D16( p.x );
      nearest_point.y -= FT_26D6_16D16( p.y );

      /* if not perpendicular then compute the cross */
      FT_Vector_NormLen( &direction );
      FT_Vector_NormLen( &nearest_point );

      out->cross = FT_MulFix( direction.x, nearest_point.y ) -
                   FT_MulFix( direction.y, nearest_point.x );
    }
  Exit:
    return error;
  }

  /**************************************************************************
   *
   * @Function:
   *   sdf_edge_get_min_distance
   *
   * @Description:
   *   This function find the shortest distance from the `edge' to
   *   a given `point' and assigns it to `out'.
   *
   * @Input:
   *   [TODO]
   *
   * @Return:
   *   [TODO]
   */
  static FT_Error
  sdf_edge_get_min_distance( SDF_Edge*             edge,
                             FT_26D6_Vec           point,
                             SDF_Signed_Distance*  out)
  {
    FT_Error  error = FT_Err_Ok;


    if ( !edge || !out )
    {
      error = FT_THROW( Invalid_Argument );
      goto Exit;
    }

    /* edge specific distance calculation */
    switch ( edge->edge_type ) {
    case SDF_EDGE_LINE:
      get_min_distance_line( edge, point, out );
      break;
    case SDF_EDGE_CONIC:
      get_min_distance_conic( edge, point, out );
      break;
    case SDF_EDGE_CUBIC:
      get_min_distance_cubic( edge, point, out );
      break;
    default:
        error = FT_THROW( Invalid_Argument );
    }

  Exit:
    return error;
  }

  /**************************************************************************
   *
   * @Function:
   *   sdf_contour_get_min_distance
   *
   * @Description:
   *   This function iterate through all the edges that make up
   *   the contour and find the shortest distance from a point to
   *   this contour and assigns it to `out'.
   *
   * @Input:
   *   [TODO]
   *
   * @Return:
   *   [TODO]
   */
  static FT_Error
  sdf_contour_get_min_distance( SDF_Contour*          contour,
                                FT_26D6_Vec           point,
                                SDF_Signed_Distance*  out)
  {
    FT_Error             error  = FT_Err_Ok;
    SDF_Signed_Distance  min_dist = max_sdf;
    SDF_Edge*            edge_list;


    if ( !contour || !out )
    {
      error = FT_THROW( Invalid_Argument );
      goto Exit;
    }

    edge_list = contour->edges;

    /* iterate through all the edges manually */
    while ( edge_list ) {
      SDF_Signed_Distance  current_dist = max_sdf;
      FT_16D16             diff;
    
    
      FT_CALL( sdf_edge_get_min_distance( 
               edge_list,
               point, &current_dist ) );

      if ( current_dist.distance >= 0 )
      {
        diff = current_dist.distance - min_dist.distance;


        if ( FT_ABS(diff ) < CORNER_CHECK_EPSILON )
          min_dist = resolve_corner( min_dist, current_dist );
        else if ( diff < 0 )
          min_dist = current_dist;
      }
      else
      {
        FT_TRACE0(( "sdf_contour_get_min_distance: Overflowed.\n" ));
      }
    
      edge_list = edge_list->next;
    }

    *out = min_dist;
  Exit:
    return error;
  }

  /**************************************************************************
   *
   * @Function:
   *   sdf_generate
   *
   * @Description:
   *   This is the main function that is responsible for generating
   *   signed distance fields. The function will not align or compute
   *   the size of the `bitmap', therefore setup the `bitmap' properly
   *   and transform the `shape' appropriately before calling this
   *   function.
   *   Currently we check all the pixels against all the contours and
   *   all the edges.
   *
   * @Input:
   *   [TODO]
   *
   * @Return:
   *   [TODO]
   */
  static FT_Error
  sdf_generate( const SDF_Params  internal_params,
                const SDF_Shape*  shape,
                FT_UInt           spread,
                const FT_Bitmap*  bitmap )
  {
    FT_Error   error = FT_Err_Ok;
    FT_UInt    width = 0;
    FT_UInt    rows  = 0;
    FT_UInt    x     = 0; /* used to loop in x direction i.e. width */
    FT_UInt    y     = 0; /* used to loop in y direction i.e. rows  */
    FT_UInt    sp_sq = 0; /* `spread' * `spread' int 16.16 fixed    */

    FT_Short*  buffer;

    if ( !shape || !bitmap )
    {
      error = FT_THROW( Invalid_Argument );
      goto Exit;
    }

    if ( spread < MIN_SPREAD || spread > MAX_SPREAD )
    {
      error = FT_THROW( Invalid_Argument );
      goto Exit;
    }

    width  = bitmap->width;
    rows   = bitmap->rows;
    buffer = (FT_Short*)bitmap->buffer;

    if ( USE_SQUARED_DISTANCES )
      sp_sq = FT_INT_16D16( spread * spread );
    else
      sp_sq = FT_INT_16D16( spread );

    if ( width == 0 || rows == 0 )
    {
      FT_TRACE0(( "[sdf] sdf_generate:\n"
                  "      Cannot render glyph with width/height == 0\n"
                  "      (width, height provided [%d, %d])", width, rows ));
      error = FT_THROW( Cannot_Render_Glyph );
      goto Exit;
    }

    /* loop through all the rows */
    for ( y = 0; y < rows; y++ )
    {
      /* loop through all the pixels of a row */
      for ( x = 0; x < width; x++ )
      {
        /* `grid_point' is the current pixel position */
        /* our task is to find the shortest distance  */
        /* from this point to the entire shape.       */
        FT_26D6_Vec          grid_point = zero_vector;
        SDF_Signed_Distance  min_dist   = max_sdf;
        SDF_Contour*         contour_list;
        FT_UInt              index;
        FT_Short             value;


        grid_point.x = FT_INT_26D6( x );
        grid_point.y = FT_INT_26D6( y );

        /* This `grid_point' is at the corner, but we */
        /* use the center of the pixel.               */
        grid_point.x += FT_INT_26D6( 1 ) / 2;
        grid_point.y += FT_INT_26D6( 1 ) / 2;

        contour_list = shape->contours;

        index = ( rows - y - 1 ) * width + x;

        /* iterate through all the contours manually */
        while ( contour_list ) {
          SDF_Signed_Distance  current_dist = max_sdf;


          FT_CALL( sdf_contour_get_min_distance( 
                   contour_list,
                   grid_point, &current_dist ) );

          if ( current_dist.distance < min_dist.distance )
            min_dist = current_dist;

          contour_list = contour_list->next;
        }

        /* [OPTIMIZATION]: if (min_dist > sp_sq) then simply clamp  */
        /*                 the value to spread to avoid square_root */

        /* clamp the values to spread */
        if ( min_dist.distance > sp_sq )
          min_dist.distance = sp_sq;

        /* square_root the values and fit in a 6.10 fixed point */
        if ( USE_SQUARED_DISTANCES )
          min_dist.distance = square_root( min_dist.distance );

        if ( internal_params.orientation == FT_ORIENTATION_FILL_LEFT )
          min_dist.sign = -min_dist.sign;

        min_dist.distance /= 64; /* convert from 16.16 to 22.10 */
        value = min_dist.distance & 0x0000FFFF; /* truncate to 6.10 */
        value *= min_dist.sign;

        buffer[index] = value;
      }
    }

  Exit:
    return error;
  }

  /**************************************************************************
   *
   * @Function:
   *   sdf_generate_bounding_box
   *
   * @Description:
   *   This function does basically the same thing as the above 
   *   `sdf_generate' but more efficiently.
   *   Instead of checking all the pixels against all the edges, we loop
   *   through all the edges and only check the pixels around the control
   *   box of the edge, the control box is increased by the spread in all
   *   all the directions. Anything outside the control box will naturally
   *   be more than the `spread' and shouldn't be computed.
   *   Lastly to determine the sign of unchecked pixels we do a single pass
   *   of all the rows starting with a + sign and flipping when we come
   *   across a - sign and continue.
   *   This also eliminate the chance of overflow because we only check the
   *   proximity of the curve. Therefore we can use squared distanced
   *   safely.
   *
   * @Input:
   *   [TODO]
   *
   * @Return:
   *   [TODO]
   */
  static FT_Error
  sdf_generate_bounding_box( const SDF_Params  internal_params,
                             const SDF_Shape*  shape,
                             FT_UInt           spread,
                             const FT_Bitmap*  bitmap )
  {
    FT_Error      error  = FT_Err_Ok;
    FT_Memory     memory = NULL;

    FT_UInt       width, rows, i, j;
    FT_UInt       sp_sq;      /* max value to check   */

    SDF_Contour*  contours;   /* list of all contours */
    FT_Short*     buffer;     /* the bitmap buffer    */

    /* This buffer has the same size in indices as the   */
    /* bitmap buffer. When we check a pixel position for */
    /* shortest distance we keep it in this buffer.      */
    /* This way we check find out which pixel is set,    */
    /* and also determine the signs properly.            */
    SDF_Signed_Distance*    dists = NULL;

    if ( !shape || !bitmap )
    {
      error = FT_THROW( Invalid_Argument );
      goto Exit;
    }

    if ( spread < MIN_SPREAD || spread > MAX_SPREAD )
    {
      error = FT_THROW( Invalid_Argument );
      goto Exit;
    }

    memory = shape->memory;
    if ( !memory ){
      error = FT_THROW( Invalid_Argument );
      goto Exit;
    }

    contours = shape->contours;
    width    = bitmap->width;
    rows     = bitmap->rows;
    buffer   = (FT_Short*)bitmap->buffer;

    if ( FT_ALLOC_MULT( dists, width, rows * sizeof(*dists) ) )
      goto Exit;

    FT_MEM_ZERO( dists, width * rows * sizeof(*dists) );

    if ( USE_SQUARED_DISTANCES )
      sp_sq = FT_INT_16D16( spread * spread );
    else
      sp_sq = FT_INT_16D16( spread );

    if ( width == 0 || rows == 0 )
    {
      FT_TRACE0(( "[sdf] sdf_generate:\n"
                  "      Cannot render glyph with width/height == 0\n"
                  "      (width, height provided [%d, %d])", width, rows ));
      error = FT_THROW( Cannot_Render_Glyph );
      goto Exit;
    }

    /* loop through all the contours */
    while ( contours ) {
      SDF_Edge*  edges = contours->edges;


      /* loop through all the edges */
      while ( edges )
      {
        FT_CBox    cbox;
        FT_Int     x, y;

        /* get the control box and increase by `spread' */
        cbox = get_control_box( *edges );
        cbox.xMin = ( cbox.xMin - 63 ) / 64 - ( FT_Pos )spread;
        cbox.xMax = ( cbox.xMax + 63 ) / 64 + ( FT_Pos )spread;
        cbox.yMin = ( cbox.yMin - 63 ) / 64 - ( FT_Pos )spread;
        cbox.yMax = ( cbox.yMax + 63 ) / 64 + ( FT_Pos )spread;

        /* now loop the pixels in the control box. */
        for ( y = cbox.yMin; y < cbox.yMax; y++  )
        {
          for ( x = cbox.xMin; x < cbox.xMax; x++  )
          {
            FT_26D6_Vec          grid_point = zero_vector;
            SDF_Signed_Distance  dist       = max_sdf;
            FT_UInt              index      = 0;


            if ( x < 0 || x >= width ) continue;
            if ( y < 0 || y >= rows )  continue;

            grid_point.x = FT_INT_26D6( x );
            grid_point.y = FT_INT_26D6( y );
            
            /* This `grid_point' is at the corner, but we */
            /* use the center of the pixel.               */
            grid_point.x += FT_INT_26D6( 1 ) / 2;
            grid_point.y += FT_INT_26D6( 1 ) / 2;
            
            index = ( rows - y - 1 ) * width + x;

            FT_CALL( sdf_edge_get_min_distance( edges,
                                                grid_point,
                                                &dist ) );

            if ( internal_params.orientation == FT_ORIENTATION_FILL_LEFT )
              dist.sign = -dist.sign;

            /* ignore if the distance is greater than spread    */
            /* otherwise it creates artifacts due to wrong sign */
            if ( dist.distance > sp_sq ) continue;

            /* square_root the values and fit in a 6.10 fixed point */
            if ( USE_SQUARED_DISTANCES )
              dist.distance = square_root( dist.distance );

            /* check weather the pixel is set or not */
            if ( dists[index].sign == 0 )
              dists[index] = dist;
            else if ( dists[index].distance > dist.distance )
              dists[index] = dist;
            else if ( FT_ABS(dists[index].distance - dist.distance  ) < CORNER_CHECK_EPSILON )
            {
              if ( FT_ABS( dists[index].cross ) < FT_ABS( dist.cross ) )
                dists[index] = dist;
            }
          }
        }

        edges = edges->next;
      }

      contours = contours->next;
    }

    /* final pass */
    for ( j = 0; j < rows; j++ )
    {
      /* We assume the starting pixel of each row */
      /* will be outside.                         */
      FT_Char  current_sign = -1;
      FT_UInt  index;

      for ( i = 0; i < width; i++ )
      {
        index = j * width + i;

        /* if the pixel is not set that means it's */
        /* shortest distance is more than spread   */
        if ( dists[index].sign == 0 )
          dists[index].distance = FT_INT_16D16( spread );
        else
          current_sign = dists[index].sign;

        /* clamp the values */
        if ( dists[index].distance > FT_INT_16D16( spread ) )
          dists[index].distance = FT_INT_16D16( spread );

        /* convert from 16.16 to 6.10 */
        dists[index].distance /= 64;

        buffer[index] = (FT_Short)dists[index].distance * current_sign;
      }
    }

  Exit:
    FT_FREE( dists );
    return error;
  }

  /**************************************************************************
   *
   * @Function:
   *   sdf_generate_subdivision
   *
   * @Description:
   *   This function subdivide the shape into a number of straight lines
   *   and then simply use the above `sdf_generate_bounding_box' to generate
   *   the SDF.
   *   Note: After calling this function the `shape' will no longer have the
   *         original edges, it will only contain lines.
   *
   * @Input:
   *   [TODO]
   *
   * @Return:
   *   [TODO]
   */
  static FT_Error
  sdf_generate_subdivision( const SDF_Params  internal_params,
                            SDF_Shape*        shape,
                            FT_UInt           spread,
                            const FT_Bitmap*  bitmap )
  {
    FT_Error   error = FT_Err_Ok;

    FT_CALL( split_sdf_shape( shape ) );
    FT_CALL( sdf_generate_bounding_box( internal_params,
                                        shape, spread, bitmap ) );

  Exit:
    return error;
  }


  /**************************************************************************
   *
   * @Function:
   *   sdf_generate_coarse_grid
   *
   * @Description:
   *   The function divide the entire bitmap into a coarse grid (the
   *   dimension is determined by the COARSE_GRID_DIV). Then for each
   *   coarse grid, it checks which edges a relevant. An edge is relevant
   *   if it's shortest distance from the coarse grid is less than `spread'
   *   + coarse grid max dimension in the x/y axis.
   *   After we have all the relevant edges we only check those edges for
   *   the pixels inside the coarse grid.
   *
   * @Input:
   *   [TODO]
   *
   * @Return:
   *   [TODO]
   */
  static FT_Error
  sdf_generate_coarse_grid( const SDF_Params  internal_params,
                            const SDF_Shape*  shape,
                            FT_UInt           spread,
                            const FT_Bitmap*  bitmap )
  {
    FT_Error      error = FT_Err_Ok;
    FT_Memory     memory;

    FT_UInt       width, rows;     /* width and rows of the bitmap        */
    FT_UInt       i, j;            /* iterators                           */
    FT_UInt       c_width, c_rows; /* coarse grid dimensions              */
    FT_16D16      sp_sq;           /* max value to check                  */
    FT_16D16      cg_sq;           /* max value to check for coarse grid  */
    FT_16D16      cg_diagon;

    SDF_Contour*  contours;        /* list of all contours */
    FT_Short*     buffer;          /* the bitmap buffer    */

    if ( !shape || !bitmap )
    {
      error = FT_THROW( Invalid_Argument );
      goto Exit;
    }

    if ( spread < MIN_SPREAD || spread > MAX_SPREAD )
    {
      error = FT_THROW( Invalid_Argument );
      goto Exit;
    }

    memory = shape->memory;

    contours = shape->contours;
    width    = bitmap->width;
    rows     = bitmap->rows;
    buffer   = (FT_Short*)bitmap->buffer;

    if ( width == 0 || rows == 0 )
    {
      FT_TRACE0(( "[sdf] sdf_generate:\n"
                  "      Cannot render glyph with width/height == 0\n"
                  "      (width, height provided [%d, %d])", width, rows ));
      error = FT_THROW( Cannot_Render_Glyph );
      goto Exit;
    }

    /* determine the dimension of the coarse grid */
    c_width = width / CG_DIMEN + ( width % CG_DIMEN == 0 ? 0 : 1 );
    c_rows = rows / CG_DIMEN + ( rows % CG_DIMEN == 0 ? 0 : 1 );

    cg_diagon = FT_INT_16D16( c_width * c_width ) +
                FT_INT_16D16( c_rows * c_rows );

    /* `cg_sq' is the max value for which we add an edge */
    /* to the coarse grid list.                          */
    if ( USE_SQUARED_DISTANCES )
    {
      sp_sq = FT_INT_16D16( spread * spread );
      cg_sq = sp_sq + cg_diagon;
    }
    else
    {
      sp_sq = FT_INT_16D16( spread );
      cg_sq = sp_sq + square_root( cg_diagon );
    }

    /* if the coarse grid is too small then simply */
    /* check all pixels against all the edges      */
    if ( c_width <= 1 || c_rows <= 1 )
    {
      error = sdf_generate( internal_params, shape, spread, bitmap );
      goto Exit;
    }

    /* Loop through the coarse grid and buildup the list */
    /* of edges for further checking.                    */
    for ( j = 0; j < CG_DIMEN; j++ )
    {
      for ( i = 0; i < CG_DIMEN; i++ )
      {
        FT_Int        cindex  = j * CG_DIMEN + i;
        SDF_Contour*  cont    = contours;
        SDF_Edge*     edge;
        SDF_Edge*     relevant_list;
        FT_26D6_Vec   cpoint;
        FT_BBox       sample_region;
        FT_Int        x, y;

        SDF_Signed_Distance  min_cg_dist = max_sdf;


        relevant_list = NULL;

        /* we check from the center of the coarse grid */
        cpoint.x = FT_INT_26D6( i * c_width ) + FT_INT_26D6( c_width / 2 );
        cpoint.y = FT_INT_26D6( j * c_rows ) + FT_INT_26D6( c_rows / 2 );

        /* for all contours */
        while ( cont )
        {
          edge = cont->edges;

          /* for all edges */
          while ( edge )
          {
            SDF_Signed_Distance  dist;
            SDF_Edge*            temp;


            FT_CALL( sdf_edge_get_min_distance( edge, cpoint, &dist ) );

            /* if the distance is more than `cg_sq' than discard it */
            if ( dist.distance < cg_sq )
            {
              FT_CALL( sdf_edge_new( memory, &temp ) );
              ft_memcpy( temp, edge, sizeof( *edge ) );
              
              temp->next = relevant_list;
              relevant_list = temp;
            }

            if ( dist.distance < min_cg_dist.distance )
              min_cg_dist = dist;
            else if ( FT_ABS( dist.distance - min_cg_dist.distance ) <
                      CORNER_CHECK_EPSILON )
              min_cg_dist = resolve_corner( min_cg_dist, dist );

            edge = edge->next;
          }

          cont = cont->next;
        }

        /* Now that we have the list of edges relevant for the pixels */
        /* inside a coarse grid, we only need to check those pixels   */
        /* against the list of edges.                                 */

        /* this gives the pixels inside the coarse grid */
        sample_region.xMin = i * c_width;
        sample_region.xMax = sample_region.xMin + c_width;
        sample_region.yMin = j * c_rows;
        sample_region.yMax = sample_region.yMin + c_rows;

        /* for all the pixes inside the coarse grid */
        for ( y = sample_region.yMin; y < sample_region.yMax; y++ )
        {
          for ( x = sample_region.xMin; x < sample_region.xMax; x++ )
          {
            FT_26D6_Vec          current_pos;
            SDF_Signed_Distance  min_dist = max_sdf;
            SDF_Edge*            edges = relevant_list;
            FT_Int               index = ( rows - y - 1 ) * width + x;


            if ( x < 0 || x >= width ) continue;
            if ( y < 0 || y >= rows )  continue;

            /* If there is no relevant edge for the */
            /* coarse grid then it's fare than the  */
            /* `spread'. In that case simply assign */
            /* `min_dist' = `min_cg_dist'.          */
            if ( !relevant_list )
              min_dist = min_cg_dist;

            /* we check from the center of a pixel */
            current_pos.x = FT_INT_26D6( x ) + FT_INT_26D6( 1 ) / 2;
            current_pos.y = FT_INT_26D6( y ) + FT_INT_26D6( 1 ) / 2;

            /* check the pixels only against the relevant edges */
            while ( edges )
            {
              SDF_Signed_Distance  dist;


              FT_CALL( sdf_edge_get_min_distance( edges,
                       current_pos, &dist ) );

              if ( dist.distance < min_dist.distance )
                min_dist = dist;
              else if ( FT_ABS( dist.distance - min_dist.distance ) <
                        CORNER_CHECK_EPSILON )
                min_dist = resolve_corner( min_dist, dist );

              edges = edges->next;
            }

            /* finally truncate and assign the values */
            if ( min_dist.distance > sp_sq )
              min_dist.distance = sp_sq;

            if ( USE_SQUARED_DISTANCES )
              min_dist.distance = square_root( min_dist.distance );

            if ( internal_params.orientation == FT_ORIENTATION_FILL_RIGHT )
              min_dist.sign = -min_dist.sign;

            min_dist.distance /= 64;

            buffer[index] = (FT_Short)min_dist.distance * min_dist.sign;

          }
        }

        /* release the allocated lists */
        while ( relevant_list )
        {
          edge = relevant_list;
          relevant_list = relevant_list->next;

          sdf_edge_done( memory, &edge );
        }
      }
    }

  Exit:
    return error;
  }

  /**************************************************************************
   *
   * interface functions
   *
   */

  static FT_Error
  sdf_raster_new( FT_Memory   memory,
                  FT_Raster*  araster)
  {
    FT_Error      error  = FT_Err_Ok;
    SDF_TRaster*  raster = NULL;


    *araster = 0;
    if ( !FT_ALLOC( raster, sizeof( SDF_TRaster ) ) )
    {
      raster->memory = memory;
      *araster = (FT_Raster)raster;
    }

    return error;
  }

  static void
  sdf_raster_reset( FT_Raster       raster,
                    unsigned char*  pool_base,
                    unsigned long   pool_size )
  {
    /* no use of this function */
    FT_UNUSED( raster );
    FT_UNUSED( pool_base );
    FT_UNUSED( pool_size );
  }

  static FT_Error
  sdf_raster_set_mode( FT_Raster      raster,
                       unsigned long  mode,
                       void*          args )
  {
    FT_UNUSED( raster );
    FT_UNUSED( mode );
    FT_UNUSED( args );


    return FT_Err_Ok;
  }

  static FT_Error
  sdf_raster_render( FT_Raster                raster,
                     const FT_Raster_Params*  params )
  {
    FT_Error                  error      = FT_Err_Ok;
    SDF_TRaster*              sdf_raster = (SDF_TRaster*)raster;
    FT_Outline*               outline    = NULL;
    const SDF_Raster_Params*  sdf_params = (const SDF_Raster_Params*)params;

    FT_Memory                 memory     = NULL;
    SDF_Shape*                shape      = NULL;
    SDF_Params                internal_params;


    /* check for valid arguments */
    if ( !sdf_raster || !sdf_params )
    {
      error = FT_THROW( Invalid_Argument );
      goto Exit;
    }

    outline = (FT_Outline*)sdf_params->root.source;

    /* check if the outline is valid or not */
    if ( !outline )
    {
      error = FT_THROW( Invalid_Outline );
      goto Exit;
    }

    /* if the outline is empty, return */
    if ( outline->n_points <= 0 || outline->n_contours <= 0 )
      goto Exit;

    /* check if the outline has valid fields */
    if ( !outline->contours || !outline->points )
    {
      error = FT_THROW( Invalid_Outline );
      goto Exit;
    }

    /* check if spread is set properly */
    if ( sdf_params->spread > MAX_SPREAD ||
         sdf_params->spread < MIN_SPREAD )
    {
      FT_TRACE0(( 
        "[sdf] sdf_raster_render:\n"
        "      The `spread' field of `SDF_Raster_Params' is invalid,\n"
        "      the value of this field must be within [%d, %d].\n"
        "      Also, you must pass `SDF_Raster_Params' instead of the\n"
        "      default `FT_Raster_Params' while calling this function\n"
        "      and set the fields properly.\n"
        , MIN_SPREAD, MAX_SPREAD) );
      error = FT_THROW( Invalid_Argument );
      goto Exit;
    }

    memory = sdf_raster->memory;
    if ( !memory )
    {
      FT_TRACE0(( "[sdf] sdf_raster_render:\n"
                  "      Raster not setup properly, "
                  "unable to find memory handle.\n" ));
      error = FT_THROW( Invalid_Handle );
      goto Exit;
    }

    /* setup the params */
    internal_params.orientation = FT_Outline_Get_Orientation( outline );
    internal_params.flip_sign = 0;
    internal_params.flip_y = 0;

    FT_CALL( sdf_shape_new( memory, &shape ) );

    FT_CALL( sdf_outline_decompose( outline, shape ) );

    FT_CALL( sdf_generate_subdivision( internal_params, shape, sdf_params->spread,
                           sdf_params->root.target ) );

    if ( shape )
      sdf_shape_done( &shape );

  Exit:
    return error;
  }

  static void
  sdf_raster_done( FT_Raster  raster )
  {
    FT_Memory  memory = (FT_Memory)((SDF_TRaster*)raster)->memory;


    FT_FREE( raster );
  }

  FT_DEFINE_RASTER_FUNCS(
    ft_sdf_raster,

    FT_GLYPH_FORMAT_OUTLINE,

    (FT_Raster_New_Func)      sdf_raster_new,       /* raster_new      */
    (FT_Raster_Reset_Func)    sdf_raster_reset,     /* raster_reset    */
    (FT_Raster_Set_Mode_Func) sdf_raster_set_mode,  /* raster_set_mode */
    (FT_Raster_Render_Func)   sdf_raster_render,    /* raster_render   */
    (FT_Raster_Done_Func)     sdf_raster_done       /* raster_done     */
  )

/* END */